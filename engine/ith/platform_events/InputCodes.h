#pragma once

// The ids of input devices and their controls: keys, mouse buttons, gamepad buttons and axes
// (ADR-0013). Device events (Event.h) carry them, and the input module routes them and names them
// for configuration files.
//
// The numeric values of Key, MouseButton, GamepadButton, GamepadAxis and GamepadType are
// **stable**: they are never renumbered or reused, because saved bindings and recordings may hold
// them. New controls get new values. The binding vocabulary built on them (the stable names such
// as `Keyboard.W`, the 1D mouse axes and the 2D controls) lives in the input module
// (ith/input/ControlNames.h, ADR-0027 rule 1).

#include <array>
#include <cstddef>
#include <cstdint>

namespace ith {

// Identifies an input device. For a gamepad it is the session id: unique while the process runs,
// never reused, never 0, and new each time the gamepad connects. For keyboard and mouse events it
// is the id the operating system gives that keyboard or mouse, or 0 when it doesn't tell; the
// input module treats every keyboard and mouse as one device.
using DeviceId = std::uint32_t;

// A key by its physical position, named after the key at that position on a US keyboard: the key
// left of S is Key::A on every layout (it prints Q on AZERTY). Game bindings use it, so W/A/S/D
// stays in place on any layout. The values are the USB HID keyboard usage ids.
// NOLINTNEXTLINE(performance-enum-size): 16 bits leave room for keys beyond the HID keyboard page
enum class Key : std::uint16_t {
    None = 0,
    A = 4,
    B = 5,
    C = 6,
    D = 7,
    E = 8,
    F = 9,
    G = 10,
    H = 11,
    I = 12,
    J = 13,
    K = 14,
    L = 15,
    M = 16,
    N = 17,
    O = 18,
    P = 19,
    Q = 20,
    R = 21,
    S = 22,
    T = 23,
    U = 24,
    V = 25,
    W = 26,
    X = 27,
    Y = 28,
    Z = 29,
    Digit1 = 30,
    Digit2 = 31,
    Digit3 = 32,
    Digit4 = 33,
    Digit5 = 34,
    Digit6 = 35,
    Digit7 = 36,
    Digit8 = 37,
    Digit9 = 38,
    Digit0 = 39,
    Enter = 40,
    Escape = 41,
    Backspace = 42,
    Tab = 43,
    Space = 44,
    Minus = 45,
    Equals = 46,
    LeftBracket = 47,
    RightBracket = 48,
    Backslash = 49,
    NonUsHash = 50, // ISO keyboards: the key left of Enter
    Semicolon = 51,
    Apostrophe = 52,
    Grave = 53,
    Comma = 54,
    Period = 55,
    Slash = 56,
    CapsLock = 57,
    F1 = 58,
    F2 = 59,
    F3 = 60,
    F4 = 61,
    F5 = 62,
    F6 = 63,
    F7 = 64,
    F8 = 65,
    F9 = 66,
    F10 = 67,
    F11 = 68,
    F12 = 69,
    PrintScreen = 70,
    ScrollLock = 71,
    Pause = 72,
    Insert = 73,
    Home = 74,
    PageUp = 75,
    Delete = 76,
    End = 77,
    PageDown = 78,
    Right = 79,
    Left = 80,
    Down = 81,
    Up = 82,
    NumLock = 83,
    KeypadDivide = 84,
    KeypadMultiply = 85,
    KeypadMinus = 86,
    KeypadPlus = 87,
    KeypadEnter = 88,
    Keypad1 = 89,
    Keypad2 = 90,
    Keypad3 = 91,
    Keypad4 = 92,
    Keypad5 = 93,
    Keypad6 = 94,
    Keypad7 = 95,
    Keypad8 = 96,
    Keypad9 = 97,
    Keypad0 = 98,
    KeypadPeriod = 99,
    NonUsBackslash = 100, // ISO keyboards: the key right of left Shift
    Application = 101,    // the context menu key
    KeypadEquals = 103,
    F13 = 104,
    F14 = 105,
    F15 = 106,
    F16 = 107,
    F17 = 108,
    F18 = 109,
    F19 = 110,
    F20 = 111,
    F21 = 112,
    F22 = 113,
    F23 = 114,
    F24 = 115,
    KeypadComma = 133,
    International1 = 135, // Japanese keyboards: Ro
    International2 = 136, // Japanese keyboards: Katakana/Hiragana
    International3 = 137, // Japanese keyboards: Yen
    International4 = 138, // Japanese keyboards: Henkan
    International5 = 139, // Japanese keyboards: Muhenkan
    Lang1 = 144,          // Korean keyboards: Hangul/English
    Lang2 = 145,          // Korean keyboards: Hanja
    LeftControl = 224,
    LeftShift = 225,
    LeftAlt = 226,
    LeftSuper = 227, // Windows, Command
    RightControl = 228,
    RightShift = 229,
    RightAlt = 230,
    RightSuper = 231,
};

// One more than the largest Key value: the size of a table indexed by key.
inline constexpr std::size_t kKeyTableSize = 256;

// The modifier keys and locks active when an event happened. A set is a bitwise OR.
enum class KeyModifiers : std::uint8_t {
    None = 0,
    Shift = 1u << 0,
    Control = 1u << 1,
    Alt = 1u << 2,
    Super = 1u << 3, // Windows, Command
    CapsLock = 1u << 4,
    NumLock = 1u << 5,
};

[[nodiscard]] constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept {
    return static_cast<KeyModifiers>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

[[nodiscard]] constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) noexcept {
    return static_cast<KeyModifiers>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

// True when `set` contains every modifier of `modifiers`.
[[nodiscard]] constexpr bool hasModifiers(KeyModifiers set, KeyModifiers modifiers) noexcept {
    return (set & modifiers) == modifiers;
}

enum class MouseButton : std::uint8_t {
    None = 0,
    Left = 1,
    Right = 2,
    Middle = 3,
    Back = 4,    // the first side button
    Forward = 5, // the second side button
};

// Gamepad buttons by position, not by label: FaceSouth is A on Xbox, Cross on PlayStation and B on
// Nintendo controllers. Glyphs come from GamepadType.
enum class GamepadButton : std::uint8_t {
    None = 0,
    FaceSouth = 1,
    FaceEast = 2,
    FaceWest = 3,
    FaceNorth = 4,
    Back = 5,  // View, Select, Share, Minus
    Guide = 6, // Xbox, PS, Home
    Start = 7, // Menu, Options, Plus
    LeftStickButton = 8,
    RightStickButton = 9,
    LeftShoulder = 10,
    RightShoulder = 11,
    DpadUp = 12,
    DpadDown = 13,
    DpadLeft = 14,
    DpadRight = 15,
    Misc1 = 16, // Xbox Series Share, PlayStation microphone, Switch Pro capture
    RightPaddle1 = 17,
    LeftPaddle1 = 18,
    RightPaddle2 = 19,
    LeftPaddle2 = 20,
    Touchpad = 21, // the PlayStation touchpad click
};

// One more than the largest GamepadButton value.
inline constexpr std::size_t kGamepadButtonCount = 22;

// Gamepad axes. Sticks report -1..1 with right and up positive; triggers report 0..1. Values are
// raw: no dead zone or response curve is applied (ADR-0013 rule 6).
enum class GamepadAxis : std::uint8_t {
    None = 0,
    LeftStickX = 1,
    LeftStickY = 2,
    RightStickX = 3,
    RightStickY = 4,
    LeftTrigger = 5,
    RightTrigger = 6,
};

// One more than the largest GamepadAxis value.
inline constexpr std::size_t kGamepadAxisCount = 7;

// The kind of gamepad, for button glyphs.
enum class GamepadType : std::uint8_t {
    Unknown = 0,
    Standard = 1, // a gamepad without a known layout; use generic glyphs
    Xbox360 = 2,
    XboxOne = 3, // Xbox One and Series
    PlayStation3 = 4,
    PlayStation4 = 5,
    PlayStation5 = 6,
    SwitchPro = 7,
    SwitchJoyConLeft = 8,
    SwitchJoyConRight = 9,
    SwitchJoyConPair = 10,
    GameCube = 11,
    Steam = 12, // Steam Controller and Steam Deck
};

// A gamepad's persistent id: the same model on the same connection reports the same GUID across
// sessions, so it can key per-device settings. Not unique between two identical gamepads.
struct GamepadGuid {
    std::array<std::uint8_t, 16> bytes{};

    friend bool operator==(const GamepadGuid&, const GamepadGuid&) = default;
};

} // namespace ith
