use std::env;

pub fn is_ru() -> bool {
    is_ru_in(|k| env::var(k).ok())
}

fn is_ru_in(get: impl Fn(&str) -> Option<String>) -> bool {
    ["LC_ALL", "LC_MESSAGES", "LANG"]
        .iter()
        .find_map(|k| get(k).filter(|v| !v.is_empty()))
        .is_some_and(|v| v.starts_with("ru"))
}

pub fn new_messages(n: usize) -> String {
    new_messages_in(n, is_ru())
}

fn new_messages_in(n: usize, ru: bool) -> String {
    if !ru {
        return format!("{n} new message{}", if n == 1 { "" } else { "s" });
    }
    let word = match (n % 10, n % 100) {
        (_, 11..=14) => "новых писем",
        (1, _) => "новое письмо",
        (2..=4, _) => "новых письма",
        _ => "новых писем",
    };
    format!("{n} {word}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ru_plural() {
        let c = [(1, "1 новое письмо"), (3, "3 новых письма"), (5, "5 новых писем"), (11, "11 новых писем"), (21, "21 новое письмо")];
        for (n, s) in c {
            assert_eq!(new_messages_in(n, true), s);
        }
        assert_eq!(new_messages_in(1, false), "1 new message");
        assert_eq!(new_messages_in(3, false), "3 new messages");
    }

    #[test]
    fn lang_pick() {
        assert!(is_ru_in(|k| (k == "LANG").then(|| "ru_RU.UTF-8".into())));
        assert!(!is_ru_in(|k| match k {
            "LC_ALL" => Some("en_US.UTF-8".into()),
            "LANG" => Some("ru_RU.UTF-8".into()),
            _ => None,
        }));
        assert!(!is_ru_in(|_| None));
    }
}
