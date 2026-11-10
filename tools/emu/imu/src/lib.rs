//! A QMI8658 accelerometer, written as a virtual part.
//!
//! The firmware's tiny driver in `mosu_board_i2c.c` only needs three things:
//! `WHO_AM_I` (`0x00`) equal to `0x05`, and the six acceleration bytes at
//! `0x35..0x3A` as little-endian `int16`, scaled for the `+-4 g` range it
//! selects in `CTRL2` (8192 LSB/g).  The control-register writes are accepted
//! and ignored: this model is always in the mode the driver asked for.
//!
//! Interactions:
//!
//! * `raise` - the wrist coming up: `x=1, y=0, z=0` (one gravity along +X);
//! * `lower` - the resting pose: `x=0, y=0, z=1` (gravity along +Z);
//! * `tilt:x,y,z` - an exact acceleration in g, kept until the next input.
//!
//! The value is kept until it changes, so the firmware sees a steady wrist
//! orientation across its 8 Hz samples.

use esp_part::prelude::*;

const SCL: &str = "SCL";
const SDA: &str = "SDA";

const DEFAULT_ADDR: u32 = 0x6A;

/// The range the firmware selects: 0x01 in CTRL2 is +-4 g.
const LSB_PER_G: f32 = 8192.0;
const FULL_SCALE_G: f32 = 4.0;

/// WHO_AM_I's answer for a QMI8658.
const WHO_AM_I: u8 = 0x05;
/// The first acceleration byte; X, Y and Z follow as little-endian pairs.
const ACCEL_START: u8 = 0x35;

struct Qmi8658 {
    scl: Option<u32>,
    sda: Option<u32>,
    addr: u32,
    /// Acceleration in g, in the sensor's own axes.
    x: f32,
    y: f32,
    z: f32,
    /// Register pointer, set by the first byte of any write.
    ptr: u8,
    /// Bytes of the current read already sent, for a read the driver splits.
    read_pos: usize,
}

impl Default for Qmi8658 {
    fn default() -> Self {
        // At rest on the wrist: gravity along +Z, so the first sample the
        // firmware takes becomes its baseline.
        Self {
            scl: None,
            sda: None,
            addr: DEFAULT_ADDR,
            x: 0.0,
            y: 0.0,
            z: 1.0,
            ptr: 0,
            read_pos: 0,
        }
    }
}

impl Qmi8658 {
    /// One acceleration axis as the sensor reports it.
    fn raw(&self, value: f32) -> i16 {
        let scaled = value * LSB_PER_G;
        scaled.round().clamp(i16::MIN as f32, i16::MAX as f32) as i16
    }

    fn accel_bytes(&self) -> [u8; 6] {
        let [x0, x1] = self.raw(self.x).to_le_bytes();
        let [y0, y1] = self.raw(self.y).to_le_bytes();
        let [z0, z1] = self.raw(self.z).to_le_bytes();
        [x0, x1, y0, y1, z0, z1]
    }

    fn register(&self, reg: u8) -> u8 {
        match reg {
            0x00 => WHO_AM_I,
            ACCEL_START..=0x3A => self.accel_bytes()[(reg - ACCEL_START) as usize],
            _ => 0,
        }
    }

    fn set(&mut self, x: f32, y: f32, z: f32) {
        let clamp = |v: f32| if v.is_finite() { v.clamp(-FULL_SCALE_G, FULL_SCALE_G) } else { 0.0 };
        self.x = clamp(x);
        self.y = clamp(y);
        self.z = clamp(z);
    }

    /// `tilt:x,y,z`, three decimal numbers in g.
    fn tilt(&mut self, input: &str) -> bool {
        let Some(rest) = input.strip_prefix("tilt:") else { return false };
        let mut parts = rest.split(',');
        let (Some(x), Some(y), Some(z), None) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            return false;
        };
        let (Ok(x), Ok(y), Ok(z)) = (x.parse::<f32>(), y.parse::<f32>(), z.parse::<f32>()) else {
            return false;
        };
        self.set(x, y, z);
        true
    }
}

impl Device for Qmi8658 {
    const NAME: &'static str = "qmi8658";
    const ON_BUS: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.scl = config.pins.get(SCL).copied();
        self.sda = config.pins.get(SDA).copied();
        if config.addr != 0 {
            self.addr = config.addr;
        }
    }

    fn claims(&self) -> Vec<Claim> {
        vec![Claim {
            what: Some(claim::What::I2c(I2cClaim {
                addr: self.addr,
                scl_pin: self.scl,
                sda_pin: self.sda,
            })),
        }]
    }

    fn on_input(&mut self, name: &str) {
        if self.tilt(name) {
            return;
        }
        match name {
            "raise" => self.set(1.0, 0.0, 0.0),
            "lower" => self.set(0.0, 0.0, 1.0),
            _ => {} // Not an interaction this part has.
        }
    }

    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let mut reply =
            BusReply { correlation_id: request.correlation_id, op: BusOp::I2c as i32, ..Default::default() };
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        if i2c.addr != self.addr {
            return Some(reply); // NACK, as an empty address does.
        }

        // The driver writes a register, then reads it in a second
        // transaction, so keep the pointer across calls.
        if let Some(&reg) = i2c.write.first() {
            self.ptr = reg;
            self.read_pos = 0;
        }
        reply.ack = true;

        if i2c.read_len == 0 {
            return Some(reply); // CTRL writes, or a probe.
        }
        if !i2c.continues {
            self.read_pos = 0;
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
                .set("x", self.x)
                .set("y", self.y)
                .set("z", self.z),
        )
    }
}

esp_part::export_part!(Qmi8658, buffer = 512);

#[cfg(test)]
mod tests {
    use super::*;

    fn wired() -> Qmi8658 {
        let mut imu = Qmi8658::default();
        imu.configure(&PartConfig {
            pins: [(SCL.to_string(), 14), (SDA.to_string(), 15)].into(),
            addr: 0x6A,
        });
        imu
    }

    fn request(addr: u32, write: &[u8], read_len: u32) -> BusRequest {
        BusRequest {
            correlation_id: 3,
            op: Some(bus_request::Op::I2c(I2cTransaction {
                bus: 0,
                addr,
                write: write.to_vec(),
                read_len,
                continues: false,
            })),
        }
    }

    fn read(imu: &mut Qmi8658, reg: u8, len: u32) -> Vec<u8> {
        let reply = imu.on_bus(&request(imu.addr, &[reg], len)).unwrap();
        assert!(reply.ack);
        reply.data
    }

    #[test]
    fn the_who_am_i_is_the_one_the_driver_wants() {
        assert_eq!(read(&mut wired(), 0x00, 1), [0x05]);
    }

    #[test]
    fn at_rest_gravity_is_one_g_along_z() {
        assert_eq!(read(&mut wired(), 0x35, 6), [0, 0, 0, 0, 0, 0x20], "8192 = 0x2000");
    }

    #[test]
    fn raise_and_lower_turn_gravity_around() {
        let mut imu = wired();
        imu.on_input("raise");
        assert_eq!(read(&mut imu, 0x35, 6), [0, 0x20, 0, 0, 0, 0]);
        imu.on_input("lower");
        assert_eq!(read(&mut imu, 0x35, 6), [0, 0, 0, 0, 0, 0x20]);
    }

    #[test]
    fn a_tilt_sets_all_three_axes_and_clamps_to_the_range() {
        let mut imu = wired();
        imu.on_input("tilt:0.5,-1,0.25");
        // 0.5 g = 4096 = 0x1000, -1 g = -8192 = 0xE000, 0.25 g = 2048 = 0x0800.
        assert_eq!(read(&mut imu, 0x35, 6), [0, 0x10, 0x00, 0xE0, 0x00, 0x08]);
        imu.on_input("tilt:9,0,0");
        assert_eq!((imu.x, imu.y, imu.z), (4.0, 0.0, 0.0), "clamped to +-4 g");
        for odd in ["tilt:", "tilt:1,2", "tilt:1,2,3,4", "tilt:a,b,c", "tilt:1,2"] {
            imu.on_input(odd);
        }
        assert_eq!((imu.x, imu.y, imu.z), (4.0, 0.0, 0.0), "bad input changes nothing");
    }

    #[test]
    fn the_value_is_kept_between_reads() {
        let mut imu = wired();
        imu.on_input("raise");
        assert_eq!(read(&mut imu, 0x35, 6), read(&mut imu, 0x35, 6));
    }

    #[test]
    fn a_separate_transmit_then_receive_uses_the_pointer() {
        let mut imu = wired();
        let wrote = imu.on_bus(&request(0x6A, &[0x35], 0)).unwrap();
        assert!(wrote.ack && wrote.data.is_empty());
        let reply = imu.on_bus(&request(0x6A, &[], 6)).unwrap();
        assert_eq!(reply.data, [0, 0, 0, 0, 0, 0x20]);
    }

    #[test]
    fn a_split_read_continues_where_it_stopped() {
        let mut imu = wired();
        assert_eq!(read(&mut imu, 0x35, 2), [0, 0]);
        let mut rest = request(0x6A, &[], 4);
        if let Some(bus_request::Op::I2c(i2c)) = &mut rest.op {
            i2c.continues = true;
        }
        assert_eq!(imu.on_bus(&rest).unwrap().data, [0, 0, 0, 0x20]);
    }

    #[test]
    fn control_writes_are_ignored_and_acknowledged() {
        let mut imu = wired();
        for (reg, value) in [(0x02u8, 0x60u8), (0x03, 0x1D), (0x08, 0x01)] {
            let reply = imu.on_bus(&request(0x6A, &[reg, value], 0)).unwrap();
            assert!(reply.ack);
        }
        assert_eq!(read(&mut imu, 0x35, 6), [0, 0, 0, 0, 0, 0x20]);
    }

    #[test]
    fn it_claims_its_address_on_the_watchs_pins() {
        assert_eq!(wired().claims(), [Claim {
            what: Some(claim::What::I2c(I2cClaim {
                addr: 0x6A,
                scl_pin: Some(14),
                sda_pin: Some(15),
            })),
        }]);
    }
}
