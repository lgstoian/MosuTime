//! A passive CO5300 / SH8601 QSPI panel.
//!
//! It claims the panel's chip select so the emulator routes every QSPI
//! transfer to it and records it with `--cosim-log`, but it never answers:
//! the panel is write-only as far as this firmware is concerned.  The host
//! reconstructs the framebuffer from the opcode/data stream.

use esp_part::prelude::*;

#[derive(Default)]
struct Panel {
    cs: Option<u32>,
    clk: Option<u32>,
    d0: Option<u32>,
    d1: Option<u32>,
    d2: Option<u32>,
    d3: Option<u32>,
    transfers: u32,
}

impl Device for Panel {
    const NAME: &'static str = "co5300";
    const ON_BUS: bool = true;

    fn configure(&mut self, config: &PartConfig) {
        self.cs = config.pins.get("CS").copied();
        self.clk = config.pins.get("SCK").copied();
        self.d0 = config.pins.get("D0").copied();
        self.d1 = config.pins.get("D1").copied();
        self.d2 = config.pins.get("D2").copied();
        self.d3 = config.pins.get("D3").copied();
    }

    fn claims(&self) -> Vec<Claim> {
        let spi = SpiClaim {
            cs_pin: self.cs,
            clk_pin: self.clk,
            mosi_pin: self.d0,
            miso_pin: self.d1,
            wp_pin: self.d2,
            hd_pin: self.d3,
            ..Default::default()
        };
        vec![Claim {
            what: Some(claim::What::Spi(spi)),
        }]
    }

    fn on_bus(&mut self, request: &BusRequest) -> Option<BusReply> {
        self.transfers = self.transfers.wrapping_add(1);

        if let Some(bus_request::Op::Spi(spi)) = &request.op {
            if spi.rx_len > 0 {
                return Some(BusReply {
                    correlation_id: request.correlation_id,
                    ack: true,
                    data: vec![0xFF; spi.rx_len as usize],
                    op: BusOp::Spi as i32,
                });
            }
        }
        None
    }

    fn state(&self) -> Option<State> {
        Some(State::new().set("transfers", self.transfers))
    }
}

// LVGL flushes 50 display lines at a time: 410 * 50 * 2 = 41 000 bytes.  The
// bus is configured for 82 000, so keep room for the largest transfer.
esp_part::export_part!(Panel, buffer = 262144);
