//! `disc-remuxer info <path>` — thin CLI shell over
//! [`disc_dvd::ops::info`].
//!
//! Detects the disc type, then (for DVD) opens the source and hands the
//! reader to the op, which dumps everything libdvdread tells us about the
//! disc to stdout. Bluray / UHD are not yet implemented.

use std::path::Path;

use anyhow::{anyhow, Context, Result};

use disc_core::{detect_disc_type, DiscType};
use disc_dvd::DvdSource;

pub fn run(path: &Path) -> Result<()> {
    let disc_type = detect_disc_type(path).context("detect_disc_type")?;
    log::info!(
        "detected disc type: {} at {}",
        disc_type.as_str(),
        path.display()
    );

    match disc_type {
        DiscType::Dvd => {
            let source = DvdSource::open(path).context("DvdSource::open")?;
            let mut out = std::io::stdout().lock();
            disc_dvd::ops::info::run(source.reader(), &mut out)
        }
        DiscType::Bluray | DiscType::UltraHd => Err(anyhow!(
            "{} support not yet implemented",
            disc_type.as_str()
        )),
    }
}
