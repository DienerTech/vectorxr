// An error the user can act on, which the page shows in the app's language:
// a code from app/src/i18n/locales/en/errors.json, the values its message
// names, and the English text, which is what logs, bug reports and an older
// page show. Diagnostics stay plain English strings.
//
// The marker wraps the code and values in U+0001 so it cannot be mistaken
// for text: errorText (app/src/lib/errors.ts) translates a string that is
// exactly one marker followed by its message, and strips the marker from any
// other (an error another error's text was built around).

use serde::Serialize;
use std::collections::BTreeMap;

const MARKER: char = '\u{1}';

#[derive(Serialize)]
struct Coded<'a> {
    code: &'a str,
    params: BTreeMap<&'a str, String>,
    message: &'a str,
}

pub fn user_error(code: &str, params: &[(&'static str, String)], message: String) -> String {
    let coded = Coded {
        code,
        params: params.iter().cloned().collect(),
        message: &message,
    };
    match serde_json::to_string(&coded) {
        Ok(json) => format!("{MARKER}vxr:{json}{MARKER}{message}"),
        Err(_) => message,
    }
}

// The English text of an error, with any marker removed.
#[cfg(test)]
pub fn plain(error: &str) -> String {
    let mut out = String::new();
    let mut rest = error;
    while let Some(start) = rest.find(MARKER) {
        out.push_str(&rest[..start]);
        let after = &rest[start + MARKER.len_utf8()..];
        match after.find(MARKER) {
            Some(end) => rest = &after[end + MARKER.len_utf8()..],
            None => {
                rest = after;
                break;
            }
        }
    }
    out.push_str(rest);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_the_english_text_after_the_code() {
        let error = user_error(
            "configParse",
            &[("path", "C:\\settings.json".into()), ("detail", "expected u32".into())],
            "Unable to parse C:\\settings.json: expected u32".into(),
        );
        assert!(error.starts_with("\u{1}vxr:{\"code\":\"configParse\""));
        assert!(error.contains("\"path\":\"C:\\\\settings.json\""));
        assert_eq!(plain(&error), "Unable to parse C:\\settings.json: expected u32");
        assert_eq!(plain(&format!("{error}. Kept.")), "Unable to parse C:\\settings.json: expected u32. Kept.");
    }
}
