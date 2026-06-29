//! zig-implant entry point. The console and silent (GUI-subsystem) variants
//! share this root source; the subsystem is a build property (see build.zig).

const std = @import("std");
const agent = @import("agent.zig");

pub fn main() void {
    agent.run() catch {};
}
