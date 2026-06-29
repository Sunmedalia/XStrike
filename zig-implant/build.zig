const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Console variant (dev — shows a cmd window).
    const console_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        // Strip debug info / PDB reference / source paths from the release exe
        // (mirrors the Rust implant's stripped release profile). Keeps the
        // binary small and free of build-account/path telltales.
        .strip = true,
    });
    // Embed PE VERSIONINFO + manifest ("System Update Helper") so the exe's
    // metadata reads as an ordinary application, not a stripped binary. Mirrors
    // crates/implant/build.rs (winres).
    console_mod.addWin32ResourceFile(.{ .file = b.path("resource.rc") });
    const exe_console = b.addExecutable(.{
        .name = "zig-implant",
        .root_module = console_mod,
    });
    b.installArtifact(exe_console);

    // Silent variant: GUI subsystem, no console window. Same source — only the
    // subsystem differs. This is the operator-deployed form (like
    // ruststrike-implant-silent): click-to-run without popping a cmd window.
    const silent_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .strip = true,
    });
    silent_mod.addWin32ResourceFile(.{ .file = b.path("resource.rc") });
    const exe_silent = b.addExecutable(.{
        .name = "zig-implant-silent",
        .root_module = silent_mod,
    });
    exe_silent.subsystem = .windows;
    b.installArtifact(exe_silent);
}
