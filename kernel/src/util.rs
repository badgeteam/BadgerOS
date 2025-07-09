use alloc::string::String;

use crate::bindings::error::EResult;

/// Try to parse a null-terminated UTF-16-LE string.
pub fn parse_utf16_le(raw: &[u8]) -> EResult<String> {
    let mut buf = String::new();

    let mut iter = raw.iter();
    while let Some(low) = iter.next()
        && let Some(high) = iter.next()
    {
        let ord = ((*high as u16) << 8) | (*low as u16);
        if ord == 0 {
            break;
        }
        buf.try_reserve(buf.len() + 1)?;
        buf.push(char::from_u32(ord as u32).unwrap_or(char::REPLACEMENT_CHARACTER));
    }

    Ok(buf)
}

/// Try to parse a UUID from a string.
pub fn parse_uuid_str(raw: &str) -> Option<u128> {
    let mut raw = raw.trim_ascii();

    // Trim the optional () or {} wrapper.
    if (raw.starts_with('(') && raw.ends_with(')')) || (raw.starts_with('{') && raw.ends_with('}'))
    {
        raw = &raw[1..raw.len() - 1];
    }

    // Assert correct format.
    let raw = raw.as_ascii()?.as_bytes();
    if raw.len() == 36 {
        if raw[8] != b'-' || raw[13] != b'-' || raw[18] != b'-' || raw[23] != b'-' {
            return None;
        }
    } else if raw.len() != 32 {
        return None;
    }

    // Parse the hexadecimal.
    let mut tmp = 0u128;
    for char in raw {
        let char = *char;
        if char >= b'0' && char <= b'9' {
            tmp <<= 4;
            tmp += (char - b'0') as u128;
        } else if char >= b'a' && char <= b'f' {
            tmp <<= 4;
            tmp += (0xa + char - b'a') as u128;
        } else if char >= b'A' && char <= b'F' {
            tmp <<= 4;
            tmp += (0xa + char - b'A') as u128;
        } else if char != b'-' {
            return None;
        }
    }

    Some(tmp)
}
