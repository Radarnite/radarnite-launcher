//! Startup timing for measurements (cargo feature "startup-trace", never in releases): one stderr line per phase,
//! in ms since main() started; on Windows the first line also gives the time from process creation to main().

#[cfg(feature = "startup-trace")]
mod imp {
    use std::sync::{Mutex, OnceLock};
    use std::time::Instant;

    static T0: OnceLock<Instant> = OnceLock::new();
    // kept in memory and written once (flush): a write per phase to a redirected stderr distorts the timings
    static LINES: Mutex<String> = Mutex::new(String::new());

    pub fn start() {
        T0.get_or_init(Instant::now);
        #[cfg(windows)]
        push(format!("trace process-create->main {:.3} ms\n", since_create()));
    }

    fn push(s: String) {
        if let Ok(mut l) = LINES.lock() {
            l.push_str(&s);
        }
    }

    pub fn mark(name: &str) {
        let t = T0.get().map(|t| t.elapsed()).unwrap_or_default();
        push(format!("trace {:9.3} ms {}\n", t.as_secs_f64() * 1000.0, name));
    }

    /// to stderr, or to the file named by RB_TRACE_FILE (the launcher starts the backend without a console)
    pub fn flush() {
        if let Ok(mut l) = LINES.lock() {
            match std::env::var_os("RB_TRACE_FILE") {
                Some(f) => {
                    let _ = std::fs::write(f, l.as_bytes());
                }
                None => eprint!("{}", l),
            }
            l.clear();
        }
    }

    #[cfg(windows)]
    fn since_create() -> f64 {
        extern "system" {
            fn GetCurrentProcess() -> isize;
            fn GetProcessTimes(h: isize, create: *mut u64, exit: *mut u64, kernel: *mut u64, user: *mut u64) -> i32;
            fn GetSystemTimePreciseAsFileTime(t: *mut u64);
        }
        let (mut c, mut e, mut k, mut u, mut now) = (0u64, 0u64, 0u64, 0u64, 0u64);
        // SAFETY: plain Win32 calls writing FILETIMEs (two little-endian u32s) into u64s
        unsafe {
            GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u);
            GetSystemTimePreciseAsFileTime(&mut now);
        }
        now.saturating_sub(c) as f64 / 10_000.0
    }
}

#[cfg(not(feature = "startup-trace"))]
mod imp {
    #[inline(always)]
    pub fn start() {}
    #[inline(always)]
    pub fn mark(_: &str) {}
    #[inline(always)]
    pub fn flush() {}
}

pub use imp::*;
