//! Minimal standard-error logger (placeholder until the logging design of
//! the next step): `LEVEL target: message`.

use log::{LevelFilter, Log, Metadata, Record};

struct StderrLogger;

impl Log for StderrLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= log::max_level()
    }

    fn log(&self, record: &Record) {
        if self.enabled(record.metadata()) {
            eprintln!("{:<5} {}: {}", record.level(), record.target(), record.args());
        }
    }

    fn flush(&self) {}
}

pub fn init(level: LevelFilter) {
    static LOGGER: StderrLogger = StderrLogger;
    log::set_logger(&LOGGER).expect("logger installed twice");
    log::set_max_level(level);
}
