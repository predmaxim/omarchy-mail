//! Suspend detection: CLOCK_BOOTTIME keeps counting while the machine sleeps, CLOCK_MONOTONIC stops.

use std::time::Duration;

const TICK: Duration = Duration::from_secs(5);
/// Shorter sleeps go unnoticed.
const MIN_SLEEP: Duration = Duration::from_secs(5);

fn clock(id: libc::clockid_t) -> Duration {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: `t` is a valid timespec for the call to fill in.
    unsafe { libc::clock_gettime(id, &mut t) };
    Duration::new(t.tv_sec.max(0) as u64, t.tv_nsec.clamp(0, 999_999_999) as u32)
}

/// Total time asleep since boot.
fn gap() -> Duration {
    clock(libc::CLOCK_BOOTTIME).saturating_sub(clock(libc::CLOCK_MONOTONIC))
}

pub fn slept(prev_gap: Duration, gap: Duration) -> bool {
    gap.saturating_sub(prev_gap) > MIN_SLEEP
}

/// Calls `on_wake` within a tick after every sleep.
pub fn spawn(on_wake: impl Fn() + Send + 'static) {
    std::thread::spawn(move || {
        let mut prev = gap();
        loop {
            std::thread::sleep(TICK);
            let now = gap();
            if slept(prev, now) {
                on_wake();
            }
            prev = now;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wake_detects_jump() {
        let s = Duration::from_secs;
        assert!(slept(s(1), s(30)));
        assert!(!slept(s(1), s(2)));
        assert!(!slept(s(30), s(1)));
    }
}
