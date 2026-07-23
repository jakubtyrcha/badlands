// Safe Rust mirror of the C `UiElement` batch, plus validation of the tree
// invariants the ABI documents. Everything past this module works on validated
// data, so the layout/draw code has no unsafe and no defensive branches.

pub const FLAG_DISABLED: u32 = 1 << 0;
pub const FLAG_ALIGN_RIGHT: u32 = 1 << 1;
pub const FLAG_ALIGN_CENTER: u32 = 1 << 2;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Row,
    Col,
    Panel,
    Label,
    Button,
    Spacer,
}

impl Kind {
    pub fn from_u8(v: u8) -> Option<Kind> {
        Some(match v {
            0 => Kind::Row,
            1 => Kind::Col,
            2 => Kind::Panel,
            3 => Kind::Label,
            4 => Kind::Button,
            5 => Kind::Spacer,
            _ => return None,
        })
    }

    // Does this element paint a background when bg_rgba has non-zero alpha?
    pub fn has_background(self) -> bool {
        matches!(self, Kind::Panel | Kind::Button)
    }
}

#[derive(Clone, Copy)]
pub struct Element {
    pub id: u32,
    pub kind: Kind,
    pub parent: i32,
    pub fixed: f32,
    pub grow: f32,
    pub pad: f32,
    pub gap: f32,
    pub bg_rgba: u32,
    pub fg_rgba: u32,
    pub text_off: u32,
    pub text_len: u32,
    pub flags: u32,
}

impl Element {
    pub fn text<'a>(&self, blob: &'a str) -> &'a str {
        if self.text_len == 0 {
            return "";
        }
        let start = self.text_off as usize;
        let end = start + self.text_len as usize;
        &blob[start..end]
    }

    // Emits a hit rect: anything the caller tagged with an id, plus every
    // button (so a click on a disabled button is still consumed rather than
    // falling through to the world behind the panel).
    pub fn is_interactive(&self) -> bool {
        self.id != 0 || self.kind == Kind::Button
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum ValidateError {
    BadTree,
    BadText,
}

// Validates the ABI's documented tree invariants:
//   - element 0 is the root and has parent == -1
//   - every other element's parent is in range AND strictly precedes it
//   - every kind byte is a known UiElementKind
//   - every text run lies inside the blob and on UTF-8 char boundaries
//
// Enforcing "parent precedes child" is what makes the single forward pass in
// layout::solve correct, and makes cycles unrepresentable rather than a hang.
pub fn validate(elements: &[Element], blob: &str) -> Result<(), ValidateError> {
    for (i, e) in elements.iter().enumerate() {
        if i == 0 {
            if e.parent != -1 {
                return Err(ValidateError::BadTree);
            }
        } else if e.parent < 0 || e.parent as usize >= i {
            return Err(ValidateError::BadTree);
        }
        if e.text_len > 0 {
            let start = e.text_off as usize;
            let end = start.checked_add(e.text_len as usize).ok_or(ValidateError::BadText)?;
            if end > blob.len() || !blob.is_char_boundary(start) || !blob.is_char_boundary(end) {
                return Err(ValidateError::BadText);
            }
        }
    }
    Ok(())
}
