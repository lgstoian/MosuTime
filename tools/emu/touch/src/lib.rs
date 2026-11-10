//! An FT3168 capacitive touch controller, written as a virtual part.
//!
//! The chip answers at I2C address 0x38 on the watch's SCL/SDA (GPIO 14/15)
//! and pulls INT (GPIO 38) low while a finger is on the glass.  The contract
//! is the driver this firmware ships, `esp_lcd_touch_ft5x06.c`:
//!
//! * register `0x02` is the number of touch points (0 or 1 here);
//! * registers `0x03..0x08` are one point: X high/low, Y high/low, then two
//!   bytes the driver ignores;
//! * every other write is accepted and ignored, as the real controller's
//!   threshold and mode registers are.
//!
//! Interactions are named by this part:
//!
//! * `tap`     - press for 80 ms, release;
//! * `double`  - two taps 250 ms apart (press to press), which is what the
//!               firmware's double-tap detector wants;
//! * `hold`    - press and stay pressed until `release`;
//! * `release` - release now;
//! * `set:x=<n>` / `set:y=<n>` (or `set:x=<n>,y=<n>`) - where the finger is.
//!
//! A gesture is queued in `on_input`, which is not told the time, and its
//! deadlines are turned into absolute times by the `poll` that follows, as
//! `proto/PART_ABI.md` describes.

use esp_part::prelude::*;

/// The part's own names for its pins; the board file wires them.
const SCL: &str = "SCL";
const SDA: &str = "SDA";
const INT: &str = "INT";

/// The FT3168's address.  `addr` in the board file overrides it.
const DEFAULT_ADDR: u32 = 0x38;

/// A tap is a short press; the firmware accepts up to 400 ms.
const TAP_HOLD_NS: u64 = 80_000_000;
/// The firmware's double-tap window starts at 650 ms, so 250 ms apart is safe
/// and slow enough for a 30 ms LVGL read loop to see both edges.
const TAP_GAP_NS: u64 = 250_000_000;

/// Centre of the 410 x 502 panel.
const DEFAULT_X: i32 = 205;
const DEFAULT_Y: i32 = 251;

/// One scheduled edge: press or release at an absolute emulated time.
#[derive(Clone, Copy, Debug)]
struct Edge {
    at_ns: u64,
    pressed: bool,
}

struct Ft3168 {
    /// The chip pins this part is wired to, from `PartConfig`.
    scl: Option<u32>,
    sda: Option<u32>,
    int_pin: Option<u32>,
    /// The 7-bit address it answers at.
    addr: u32,
    /// Where the finger is.
    x: i32,
    y: i32,
    /// True between a press and its release.
    pressed: bool,
    /// The INT level last driven: a pin no part drives reads high, so this
    /// starts `true` and the first press is the first message.
    reported_high: bool,
    /// Edges with absolute times, waiting for `poll` to reach them.
    schedule: Vec<Edge>,
    /// A gesture whose clock has not started: offsets from the poll that
    /// follows `on_input`.
    queued: Vec<(u64, bool)>,
    /// Register pointer, set by the first byte of any write.
    ptr: u8,
    /// How many bytes of the current read were already sent, for a read the
    /// driver splits with `continues`.
    read_pos: usize,
}

impl Default for Ft3168 {
    fn default() -> Self {
        Self {
            scl: None,
            sda: None,
            int_pin: None,
            addr: DEFAULT_ADDR,
            x: DEFAULT_X,
            y: DEFAULT_Y,
            pressed: false,
            reported_high: true,
            schedule: Vec::new(),
            queued: Vec::new(),
            ptr: 0,
            read_pos: 0,
        }
    }
}

impl Ft3168 {
    /// The register file the driver reads.  Only the point count and the
    /// first point carry anything; the threshold/mode registers it writes
    /// never need to read back.
    fn register(&self, reg: u8) -> u8 {
        if !self.pressed {
            return 0; // No point, and no coordinates to go with it.
        }
        match reg {
            0x02 => 1,
            0x03 => ((self.x >> 8) as u8) & 0x0F,
            0x04 => self.x as u8,
            0x05 => ((self.y >> 8) as u8) & 0x0F,
            0x06 => self.y as u8,
            _ => 0,
        }
    }

    fn set_x(&mut self, value: f64) {
        self.x = (value.round() as i32).clamp(0, 4095);
    }

    fn set_y(&mut self, value: f64) {
        self.y = (value.round() as i32).clamp(0, 502);
    }

    /// `set:x=<n>,y=<n>`, the two-coordinate form, before `set_value` sees it.
    fn set_both(&mut self, input: &str) -> bool {
        let Some(rest) = input.strip_prefix("set:") else { return false };
        let Some((x, y)) = rest.split_once(",y=") else { return false };
        let Some(x) = x.strip_prefix("x=") else { return false };
        let (Ok(x), Ok(y)) = (x.parse::<f64>(), y.parse::<f64>()) else { return false };
        if !x.is_finite() || !y.is_finite() {
            return false;
        }
        self.set_x(x);
        self.set_y(y);
        true
    }
}

impl Device for Ft3168 {
    const NAME: &'static str = "ft3168";
    const ON_BUS: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.scl = config.pins.get(SCL).copied();
        self.sda = config.pins.get(SDA).copied();
        self.int_pin = config.pins.get(INT).copied();
        if config.addr != 0 {
            self.addr = config.addr;
        }
    }

    fn claims(&self) -> Vec<Claim> {
        let mut claims = vec![Claim {
            what: Some(claim::What::I2c(I2cClaim {
                addr: self.addr,
                scl_pin: self.scl,
                sda_pin: self.sda,
            })),
        }];
        if let Some(pin) = self.int_pin {
            claims.push(drives(1 << pin));
        }
        claims
    }

    fn on_input(&mut self, name: &str) {
        if self.set_both(name) {
            return;
        }
        if let Some((what, value)) = set_value(name) {
            match what {
                "x" => self.set_x(value),
                "y" => self.set_y(value),
                _ => {} // Not a value this part has.
            }
            return;
        }

        let script: &[(u64, bool)] = match name {
            "tap" => &[(0, true), (TAP_HOLD_NS, false)],
            "double" => &[
                (0, true),
                (TAP_HOLD_NS, false),
                (TAP_GAP_NS, true),
                (TAP_GAP_NS + TAP_HOLD_NS, false),
            ],
            "hold" | "press" => &[(0, true)],
            "release" => &[(0, false)],
            _ => return, // Not an interaction this part has.
        };

        // A new gesture wins over one still in flight.
        self.schedule.clear();
        self.queued = script.to_vec();
    }

    fn poll(&mut self, now_ns: u64) -> Option<EmulatorMessage> {
        if !self.queued.is_empty() {
            let start = now_ns;
            self.schedule = self
                .queued
                .drain(..)
                .map(|(offset, pressed)| Edge { at_ns: start.saturating_add(offset), pressed })
                .collect();
        }

        let mut latest = None;
        while let Some(edge) = self.schedule.first().copied() {
            if edge.at_ns > now_ns {
                break;
            }
            latest = Some(edge.pressed);
            self.schedule.remove(0);
        }
        if let Some(pressed) = latest {
            self.pressed = pressed;
        }

        let pin = self.int_pin?;
        let level_high = !self.pressed;
        if self.reported_high == level_high {
            return None; // Speak only when the line moves.
        }
        self.reported_high = level_high;
        Some(drive(pin, level_high, now_ns))
    }

    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let mut reply =
            BusReply { correlation_id: request.correlation_id, op: BusOp::I2c as i32, ..Default::default() };
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply); // NACK, as an empty address does.
        }

        // A write's first byte is the register pointer; the data the driver
        // writes (thresholds, mode) is accepted and ignored.
        if let Some(&reg) = i2c.write.first() {
            self.ptr = reg;
            self.read_pos = 0;
        }
        reply.ack = true;

        if i2c.read_len == 0 {
            return Some(reply); // A probe or a plain write.
        }
        if !i2c.continues {
            self.read_pos = 0; // A new read starts at the pointer.
        }
        let n = i2c.read_len as usize;
        reply.data = (0..n)
            .map(|i| self.register(self.ptr.wrapping_add((self.read_pos + i) as u8)))
            .collect();
        self.read_pos += n;
        Some(reply)
    }

    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("pressed", self.pressed)
                .set("x", self.x)
                .set("y", self.y),
        )
    }
}

// The driver reads at most 6 * 5 = 30 bytes at a time; 512 is the usual
// margin for the request and the reply.
esp_part::export_part!(Ft3168, buffer = 512);

#[cfg(test)]
mod tests {
    use super::*;

    fn wired() -> Ft3168 {
        let mut touch = Ft3168::default();
        touch.configure(&PartConfig {
            pins: [
                (SCL.to_string(), 14),
                (SDA.to_string(), 15),
                (INT.to_string(), 38),
            ]
            .into(),
            addr: 0x38,
        });
        touch
    }

    fn request(addr: u32, write: &[u8], read_len: u32) -> BusRequest {
        BusRequest {
            correlation_id: 7,
            op: Some(bus_request::Op::I2c(I2cTransaction {
                bus: 0,
                addr,
                write: write.to_vec(),
                read_len,
                continues: false,
            })),
        }
    }

    fn read(touch: &mut Ft3168, reg: u8, len: u32) -> Vec<u8> {
        let reply = touch.on_bus(&request(touch.addr, &[reg], len)).unwrap();
        assert!(reply.ack, "the controller should ACK");
        assert_eq!(reply.correlation_id, 7);
        assert_eq!(reply.op, BusOp::I2c as i32);
        reply.data
    }

    fn drive_of(msg: &Option<EmulatorMessage>) -> (u32, bool) {
        match msg.as_ref().and_then(|m| m.payload.as_ref()) {
            Some(esp_part::proto::emulator_message::Payload::GpioDrive(d)) => (d.pin, d.level),
            other => panic!("not a GPIO drive: {other:?}"),
        }
    }

    #[test]
    fn idle_reads_no_points_and_zeroes() {
        let mut touch = wired();
        assert_eq!(read(&mut touch, 0x02, 1), [0], "no finger");
        assert_eq!(read(&mut touch, 0x03, 6), [0; 6]);
    }

    #[test]
    fn a_tap_presses_for_80_ms_and_releases() {
        let mut touch = wired();
        touch.on_input("tap");

        let (pin, level) = drive_of(&touch.poll(1_000_000_000));
        assert_eq!((pin, level), (38, false), "INT low on press");
        assert!(touch.pressed);
        assert_eq!(read(&mut touch, 0x02, 1), [1], "one point");
        assert_eq!(read(&mut touch, 0x03, 6), [0, 205, 0, 251, 0, 0]);

        assert_eq!(touch.poll(1_079_000_000), None, "still held");
        let (pin, level) = drive_of(&touch.poll(1_080_000_000));
        assert_eq!((pin, level), (38, true), "INT high on release");
        assert!(!touch.pressed);
        assert_eq!(read(&mut touch, 0x02, 1), [0]);
    }

    #[test]
    fn a_double_tap_is_two_presses_250_ms_apart() {
        let mut touch = wired();
        touch.on_input("double");

        // Press, release, press, release, at 0, 80, 250 and 330 ms.
        let mut edges = Vec::new();
        for t_ms in [0u64, 79, 80, 249, 250, 330] {
            if let Some(msg) = touch.poll(t_ms * 1_000_000) {
                let (_, level) = drive_of(&Some(msg));
                edges.push((t_ms, level));
            }
        }
        assert_eq!(edges, [(0, false), (80, true), (250, false), (330, true)]);
    }

    #[test]
    fn hold_stays_pressed_until_release() {
        let mut touch = wired();
        touch.on_input("hold");
        assert_eq!(drive_of(&touch.poll(0)).1, false);
        assert_eq!(touch.poll(5_000_000_000), None, "still down after 5 s");
        assert!(touch.pressed);
        touch.on_input("release");
        assert_eq!(drive_of(&touch.poll(5_000_000_001)).1, true);
        assert!(!touch.pressed);
    }

    #[test]
    fn set_moves_the_finger_and_clamps_to_the_glass() {
        let mut touch = wired();
        touch.on_input("hold");
        touch.poll(0);
        touch.on_input("set:x=10");
        touch.on_input("set:y=20");
        assert_eq!(read(&mut touch, 0x03, 6), [0, 10, 0, 20, 0, 0]);
        touch.on_input("set:x=100,y=300");
        assert_eq!(read(&mut touch, 0x03, 6), [0, 100, 1, 44, 0, 0], "0x012c");
        touch.on_input("set:x=-5");
        touch.on_input("set:y=502");
        assert_eq!((touch.x, touch.y), (0, 502));
        assert_eq!(touch.state().unwrap().get("pressed"), Some("true"));
    }

    #[test]
    fn a_split_read_continues_where_it_stopped() {
        let mut touch = wired();
        touch.on_input("hold");
        touch.poll(0);
        assert_eq!(read(&mut touch, 0x03, 2), [0, 205]);
        let mut rest = request(0x38, &[], 4);
        if let Some(bus_request::Op::I2c(i2c)) = &mut rest.op {
            i2c.continues = true;
        }
        let reply = touch.on_bus(&rest).unwrap();
        assert_eq!(reply.data, [0, 251, 0, 0]);
    }

    #[test]
    fn a_separate_transmit_then_receive_uses_the_pointer() {
        // mosu_board's IMU driver style: write the register, then read.
        let mut touch = wired();
        touch.on_input("hold");
        touch.poll(0);
        let wrote = touch.on_bus(&request(0x38, &[0x03], 0)).unwrap();
        assert!(wrote.ack && wrote.data.is_empty());
        let reply = touch.on_bus(&request(0x38, &[], 6)).unwrap();
        assert_eq!(reply.data, [0, 205, 0, 251, 0, 0]);
    }

    #[test]
    fn an_unrelated_address_is_nacked() {
        let mut touch = wired();
        let reply = touch.on_bus(&request(0x39, &[0x02], 1)).unwrap();
        assert!(!reply.ack);
    }

    #[test]
    fn it_claims_its_address_and_the_interrupt_line() {
        let claims = wired().claims();
        assert_eq!(claims.len(), 2);
        assert_eq!(claims[0], Claim {
            what: Some(claim::What::I2c(I2cClaim {
                addr: 0x38,
                scl_pin: Some(14),
                sda_pin: Some(15),
            })),
        });
        assert_eq!(claims[1], drives(1 << 38));
    }
}
