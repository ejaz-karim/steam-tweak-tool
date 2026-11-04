const std = @import("std");

const targets: []const std.Target.Query = &.{
    .{ .os_tag = .windows, .cpu_arch = .x86_64, .abi = .gnu },
};

const project_name = "steam-tweak-tool";

const version = "2.0.0";

const platform_name = "windows";

const arch_name = "x64";

const sources: []const []const u8 = &.{
    "src/autoupdate_disable.cpp",
    "src/cloud_disable.cpp",
    "src/delete_cloud_data.cpp",
    "src/gameupdate_appmanifest_disable.cpp",
    "src/main.cpp",
    "src/steam_chromedevtoolsprotocol.cpp",
    "src/top_level_actions.cpp",
    "src/utility.cpp",
};

const cxx_flags: []const []const u8 = &.{"-std=c++17"};

pub fn build(b: *std.Build) void {
    for (targets) |query| {
        const target = b.resolveTargetQuery(query);

        const exe_mod = b.createModule(.{
            .target = target,
            .optimize = .ReleaseFast,
            .strip = true,
            .link_libc = true,
            .link_libcpp = true,
        });

        exe_mod.addCSourceFiles(.{
            .files = sources,
            .flags = cxx_flags,
        });

        exe_mod.addIncludePath(b.path("include"));

        exe_mod.linkSystemLibrary("ws2_32", .{});

        const exe = b.addExecutable(.{
            .name = "Steam Tweak Tool",
            .root_module = exe_mod,
        });

        const installDirName = b.fmt("{s}_{s}_{s}_{s}", .{
            project_name,
            version,
            platform_name,
            arch_name,
        });

        const install = b.addInstallArtifact(exe, .{
            .dest_dir = .{
                .override = .{
                    .custom = installDirName,
                },
            },
        });

        b.getInstallStep().dependOn(&install.step);

        const installDll = b.addInstallFile(
            b.path("vendor/steam_api64.dll"),
            b.fmt("{s}/steam_api64.dll", .{installDirName}),
        );

        b.getInstallStep().dependOn(&installDll.step);
    }
}
