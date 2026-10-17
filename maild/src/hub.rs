//! Events for socket subscribers, the latest status of every account, account thread handles.

use crate::account::{Handle, Status};
use crate::db::Summary;
use serde::Serialize;
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

#[derive(Debug, Serialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub enum Event {
    Changed { folders: bool, ids: Vec<i64> },
    Status { accounts: Vec<AccountState> },
    Open { id: i64 },
    SearchRemote { token: u64, hits: Vec<Summary> },
    ActionFailed { batch: i64, error: String },
    SendFailed { outbox: i64, error: String },
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct AccountState { pub email: String, pub status: Status }

/// Shared by every thread; clones share the state.
#[derive(Clone, Default)]
pub struct Hub {
    /// One JSON line per event; `true` = a window.
    subs: Arc<Mutex<Vec<(Sender<String>, bool)>>>,
    accounts: Arc<Mutex<Vec<AccountState>>>,
    handles: Arc<Mutex<Vec<Handle>>>,
}

/// Nothing panics while holding these locks, but a poisoned one must not take the service down.
pub fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Hub {
    /// Subscribers whose receiver is gone are dropped here.
    pub fn emit(&self, e: Event) {
        let Ok(line) = serde_json::to_string(&e) else { return };
        lock(&self.subs).retain(|(tx, _)| tx.send(line.clone()).is_ok());
    }

    pub fn subscribe(&self, window: bool) -> Receiver<String> {
        let (tx, rx) = channel();
        lock(&self.subs).push((tx, window));
        rx
    }

    /// Counts a window that went away since the last `emit`.
    pub fn has_window(&self) -> bool {
        lock(&self.subs).iter().any(|(_, w)| *w)
    }

    pub fn add_handle(&self, h: Handle) {
        lock(&self.handles).push(h);
    }

    pub fn handles(&self) -> Vec<Handle> {
        lock(&self.handles).clone()
    }

    /// Emits `Status` with every account (and logs) when this one's status changed.
    pub fn set_status(&self, email: &str, status: Status) {
        {
            let mut v = lock(&self.accounts);
            match v.iter_mut().find(|a| a.email == email) {
                Some(a) if a.status == status => return,
                Some(a) => a.status = status.clone(),
                None => v.push(AccountState { email: email.into(), status: status.clone() }),
            }
            // Under the lock, so concurrent changes reach subscribers in order (accounts → subs).
            self.emit(Event::Status { accounts: v.clone() });
        }
        eprintln!("{email}: {status:?}");
    }

    /// Stops the account's threads and waits for them, then forgets its status.
    pub fn remove_handle(&self, email: &str) {
        let gone: Vec<Handle> = lock(&self.handles).extract_if(.., |h| h.email == email).collect();
        for h in gone {
            h.stop_and_join();
        }
        let mut v = lock(&self.accounts);
        v.retain(|a| a.email != email);
        self.emit(Event::Status { accounts: v.clone() });
    }

    pub fn statuses(&self) -> Vec<AccountState> {
        lock(&self.accounts).clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hub_drops_dead_subscriber() {
        let hub = Hub::default();
        let rx = hub.subscribe(true);
        assert!(hub.has_window());
        hub.emit(Event::Open { id: 1 });
        assert_eq!(rx.recv().unwrap(), r#"{"event":"open","id":1}"#);
        drop(rx);
        hub.emit(Event::Open { id: 2 });
        assert_eq!(lock(&hub.subs).len(), 0);
        assert!(!hub.has_window());
    }

    #[test]
    fn status_change_emits_all_accounts() {
        let hub = Hub::default();
        let rx = hub.subscribe(false);
        hub.set_status("a@x", Status::Online);
        hub.set_status("b@x", Status::AuthFailed);
        hub.set_status("a@x", Status::Online);
        hub.set_status("a@x", Status::Offline("down".into()));
        let got: Vec<String> = rx.try_iter().collect();
        assert_eq!(got, [
            r#"{"event":"status","accounts":[{"email":"a@x","status":"online"}]}"#,
            r#"{"event":"status","accounts":[{"email":"a@x","status":"online"},{"email":"b@x","status":"auth_failed"}]}"#,
            r#"{"event":"status","accounts":[{"email":"a@x","status":{"offline":"down"}},{"email":"b@x","status":"auth_failed"}]}"#,
        ]);
        assert_eq!(hub.statuses().len(), 2);
    }

    #[test]
    fn remove_handle_drops_status() {
        let hub = Hub::default();
        hub.set_status("a@x", Status::Online);
        let rx = hub.subscribe(false);
        hub.remove_handle("a@x");
        assert!(hub.statuses().is_empty());
        assert_eq!(rx.try_iter().collect::<Vec<_>>(), [r#"{"event":"status","accounts":[]}"#]);
    }
}
