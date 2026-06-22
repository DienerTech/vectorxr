fn main() {
    // tauri-build embeds icons/icon.ico into the Windows executable, but it does
    // not register the icon files in its own rerun-if-changed set. Without these,
    // regenerating the icons does not re-run this build script, so the rebuilt exe
    // keeps the previously embedded icon. Watch them explicitly.
    println!("cargo:rerun-if-changed=icons/icon.ico");
    println!("cargo:rerun-if-changed=icons/icon.png");

    tauri_build::build()
}
