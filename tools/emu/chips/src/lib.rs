//! The AXP2101 PMU, PCF85063 RTC and ES8311 codec, as one register-file part.
//!
//! esp-emu 1.0.0-rc2 answers an I2C port only through the virtual parts wired
//! to it: on a port that has a part, its `regfile` devices are not asked
//! (docs/guides/PARTS.md, "A device on a port a part is wired to is not asked
//! there. Parts win.").  The touch and IMU parts need the port, so the three
//! simple chips are modelled here with exactly the registers `board.json`
//! preloads.  Without them the boot loses the battery, the clock (and with it
//! the announcement path) and the codec.
//!
//! The behaviour is the emulator's `regfile`: an 8-bit register pointer, auto
//! incrementing, writes stored and read back, `fill` for registers nobody
//! wrote.  There is no chip behaviour beyond that.

use esp_part::prelude::*;

const SCL: &str = "SCL";
const SDA: &str = "SDA";

const AXP2101_ADDR: u32 = 0x34;
const PCF85063_ADDR: u32 = 0x51;
const ES8311_ADDR: u32 = 0x18;

/// One 8-bit register file.
struct RegFile {
    regs: [u8; 256],
    /// The register pointer: the first byte of each write.
    ptr: u8,
    /// Bytes of the current read already sent, for a read split with
    /// `continues`.
    read_pos: usize,
}

impl RegFile {
    fn new(fill: u8) -> Self {
        Self { regs: [fill; 256], ptr: 0, read_pos: 0 }
    }

    fn preload(mut self, pairs: &[(u8, u8)]) -> Self {
        for &(reg, value) in pairs {
            self.regs[reg as usize] = value;
        }
        self
    }

    fn write(&mut self, bytes: &[u8]) {
        let Some((&reg, data)) = bytes.split_first() else { return };
        self.ptr = reg;
        self.read_pos = 0;
        for (i, &value) in data.iter().enumerate() {
            let at = reg.wrapping_add(i as u8);
            self.regs[at as usize] = value;
            self.ptr = at.wrapping_add(1);
        }
    }

    fn read(&mut self, len: usize, continues: bool) -> Vec<u8> {
        if !continues {
            self.read_pos = 0;
        }
        let data = (0..len)
            .map(|i| self.regs[self.ptr.wrapping_add((self.read_pos + i) as u8) as usize])
            .collect();
        self.read_pos += len;
        data
    }
}

struct Chips {
    scl: Option<u32>,
    sda: Option<u32>,
    pmu: RegFile,
    rtc: RegFile,
    codec: RegFile,
}

impl Default for Chips {
    fn default() -> Self {
        Self {
            scl: None,
            sda: None,
            // The values board.json preloads: battery present and VBUS good,
            // 3.5 V; 2026-11-10 07:41:00 UTC; an ES8311 that only needs its
            // transactions acknowledged.
            pmu: RegFile::new(0xFF).preload(&[(0x00, 0x28), (0x01, 0x00), (0x34, 0x0F), (0x35, 0x3C)]),
            rtc: RegFile::new(0xFF).preload(&[
                (0x02, 0x00),
                (0x03, 0x41),
                (0x04, 0x07),
                (0x05, 0x10),
                (0x06, 0x02),
                (0x07, 0x11),
                (0x08, 0x26),
            ]),
            codec: RegFile::new(0x00),
        }
    }
}

impl Chips {
    fn regfile(&mut self, addr: u32) -> Option<&mut RegFile> {
        match addr {
            AXP2101_ADDR => Some(&mut self.pmu),
            PCF85063_ADDR => Some(&mut self.rtc),
            ES8311_ADDR => Some(&mut self.codec),
            _ => None,
        }
    }
}

impl Device for Chips {
    const NAME: &'static str = "chips";
    const ON_BUS: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.scl = config.pins.get(SCL).copied();
        self.sda = config.pins.get(SDA).copied();
    }

    fn claims(&self) -> Vec<Claim> {
        [AXP2101_ADDR, PCF85063_ADDR, ES8311_ADDR]
            .into_iter()
            .map(|addr| Claim {
                what: Some(claim::What::I2c(I2cClaim {
                    addr,
                    scl_pin: self.scl,
                    sda_pin: self.sda,
                })),
            })
            .collect()
    }

    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        let mut reply =
            BusReply { correlation_id: request.correlation_id, op: BusOp::I2c as i32, ..Default::default() };
        let Some(bus_request::Op::I2c(i2c)) = &request.op else { return Some(reply) };
        let Some(file) = self.regfile(i2c.addr) else { return Some(reply) }; // NACK

        if !i2c.write.is_empty() {
            file.write(&i2c.write);
        }
        reply.ack = true;
        if i2c.read_len > 0 {
            reply.data = file.read(i2c.read_len as usize, i2c.continues);
        }
        Some(reply)
    }

    fn state(&self) -> Option<State> {
        Some(
            State::new()
                .set("pmu-status", format!("0x{:02X}", self.pmu.regs[0x00]))
                .set("rtc-seconds", format!("0x{:02X}", self.rtc.regs[0x02]))
                .set("rtc-year", format!("0x{:02X}", self.rtc.regs[0x08])),
        )
    }
}

// A read is at most the widest register burst the codec driver asks for.
esp_part::export_part!(Chips, buffer = 512);

#[cfg(test)]
mod tests {
    use super::*;

    fn wired() -> Chips {
        let mut chips = Chips::default();
        chips.configure(&PartConfig {
            pins: [(SCL.to_string(), 14), (SDA.to_string(), 15)].into(),
            ..Default::default()
        });
        chips
    }

    fn request(addr: u32, write: &[u8], read_len: u32) -> BusRequest {
        BusRequest {
            correlation_id: 9,
            op: Some(bus_request::Op::I2c(I2cTransaction {
                bus: 0,
                addr,
                write: write.to_vec(),
                read_len,
                continues: false,
            })),
        }
    }

    fn read(chips: &mut Chips, addr: u32, reg: u8, len: u32) -> Vec<u8> {
        let reply = chips.on_bus(&request(addr, &[reg], len)).unwrap();
        assert!(reply.ack, "0x{addr:02X} should ACK");
        assert_eq!((reply.correlation_id, reply.op), (9, BusOp::I2c as i32));
        reply.data
    }

    #[test]
    fn all_three_addresses_are_claimed_on_the_watchs_pins() {
        let claims = wired().claims();
        assert_eq!(claims.len(), 3);
        for (claim, addr) in claims.iter().zip([0x34, 0x51, 0x18]) {
            assert_eq!(
                claim,
                &Claim {
                    what: Some(claim::What::I2c(I2cClaim {
                        addr,
                        scl_pin: Some(14),
                        sda_pin: Some(15),
                    })),
                }
            );
        }
    }

    #[test]
    fn the_pmu_reports_the_same_status_and_battery_as_board_json() {
        let mut chips = wired();
        assert_eq!(read(&mut chips, 0x34, 0x00, 1), [0x28], "battery present, VBUS good");
        assert_eq!(read(&mut chips, 0x34, 0x01, 1), [0x00], "not charging");
        assert_eq!(read(&mut chips, 0x34, 0x34, 2), [0x0F, 0x3C], "3900 mV");
        // The init's read-modify-write lands in the register file.
        chips.on_bus(&request(0x34, &[0x30, 0x0D], 0)).unwrap();
        assert_eq!(read(&mut chips, 0x34, 0x30, 1), [0x0D]);
    }

    #[test]
    fn the_rtc_reads_2026_11_10_07_41_utc() {
        let mut chips = wired();
        assert_eq!(
            read(&mut chips, 0x51, 0x02, 7),
            [0x00, 0x41, 0x07, 0x10, 0x02, 0x11, 0x26]
        );
    }

    #[test]
    fn the_codec_acknowledges_stores_and_reads_back() {
        let mut chips = wired();
        assert_eq!(read(&mut chips, 0x18, 0x00, 1), [0x00], "fill");
        chips.on_bus(&request(0x18, &[0x00, 0x83], 0)).unwrap();
        assert_eq!(read(&mut chips, 0x18, 0x00, 1), [0x83], "read back");
        assert_eq!(read(&mut chips, 0x18, 0x20, 1), [0x00], "untouched stays fill");
    }

    #[test]
    fn multi_byte_write_auto_increments_the_pointer() {
        let mut chips = wired();
        chips.on_bus(&request(0x51, &[0x02, 0x00, 0x41, 0x07], 0)).unwrap();
        assert_eq!(read(&mut chips, 0x51, 0x02, 3), [0x00, 0x41, 0x07]);
    }

    #[test]
    fn a_split_read_continues_where_it_stopped() {
        let mut chips = wired();
        assert_eq!(read(&mut chips, 0x51, 0x02, 3), [0x00, 0x41, 0x07]);
        let mut rest = request(0x51, &[], 4);
        if let Some(bus_request::Op::I2c(i2c)) = &mut rest.op {
            i2c.continues = true;
        }
        assert_eq!(chips.on_bus(&rest).unwrap().data, [0x10, 0x02, 0x11, 0x26]);
    }

    #[test]
    fn an_address_it_does_not_model_is_nacked() {
        let mut chips = wired();
        assert!(!chips.on_bus(&request(0x39, &[0x00], 1)).unwrap().ack);
    }
}
