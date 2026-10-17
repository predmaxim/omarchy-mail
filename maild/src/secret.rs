use std::io::Write;
use std::process::{Command, Stdio};

fn run(args: &[&str], stdin: Option<&str>) -> Result<String, String> {
    let mut child = Command::new("secret-tool")
        .args(args)
        .stdin(if stdin.is_some() { Stdio::piped() } else { Stdio::null() })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("secret-tool: {e}"))?;
    if let (Some(s), Some(mut pipe)) = (stdin, child.stdin.take()) {
        pipe.write_all(s.as_bytes()).map_err(|e| format!("secret-tool: {e}"))?;
    }
    let out = child.wait_with_output().map_err(|e| format!("secret-tool: {e}"))?;
    if !out.status.success() {
        return Err(format!("secret-tool: {}", String::from_utf8_lossy(&out.stderr).trim()));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

/// The password goes through stdin only, never argv.
pub fn store(email: &str, password: &str) -> Result<(), String> {
    let label = format!("omarchy-mail {email}");
    run(&["store", "--label", &label, "service", "omarchy-mail", "account", email], Some(password)).map(drop)
}

pub fn lookup(email: &str) -> Result<String, String> {
    let out = run(&["lookup", "service", "omarchy-mail", "account", email], None)?;
    if out.is_empty() {
        return Err(format!("no password stored for {email}"));
    }
    Ok(out)
}

pub fn clear(email: &str) -> Result<(), String> {
    run(&["clear", "service", "omarchy-mail", "account", email], None).map(drop)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Needs a reachable keyring: `cargo test -- --ignored`.
    #[test]
    #[ignore]
    fn keyring_roundtrip() {
        let e = "test@example.invalid";
        store(e, "s3cret").unwrap();
        assert_eq!(lookup(e).unwrap(), "s3cret");
        clear(e).unwrap();
        assert!(lookup(e).is_err());
    }
}
