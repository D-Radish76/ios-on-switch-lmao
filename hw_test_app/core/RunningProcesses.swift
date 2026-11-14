import Darwin

/// The processes running now, as kern.proc.all lists them to the app (its sandbox admits the sysctl)
public enum RunningProcesses {
    /// The first pid whose command name is `name`; nil when none runs or the list is refused
    public static func pid(named name: String) -> Int32? {
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
        var size = 0
        guard sysctl(&mib, 4, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: size / MemoryLayout<kinfo_proc>.stride + 8)
        size = procs.count * MemoryLayout<kinfo_proc>.stride
        guard sysctl(&mib, 4, &procs, &size, nil, 0) == 0 else { return nil }
        for proc in procs.prefix(size / MemoryLayout<kinfo_proc>.stride) {
            var comm = proc.kp_proc.p_comm
            let command = withUnsafeBytes(of: &comm) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            if command == name { return proc.kp_proc.p_pid }
        }
        return nil
    }
}
