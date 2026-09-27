import eu.darkbot.api.DarkTanos;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * End-to-end scenarios against the real browser + flash plugin, driven through the JNI
 * library the same way DarkBot's TanosAdapter does. Prints one line per metric:
 *   RESULT <name> <value>
 */
public class TanosIT {

    static final DarkTanos tanos = new DarkTanos();
    static final List<String> failures = new ArrayList<>();

    static void result(String name, Object value) {
        System.out.println("RESULT " + name + " " + value);
    }

    static void check(boolean ok, String what) {
        if (!ok) failures.add(what);
        System.out.println((ok ? "  ok   " : "  FAIL ") + what);
    }

    /** Polls isValid() like the bot's main loop until cond is true; returns ms or -1. */
    static long waitFor(BooleanSupplier cond, long timeoutMs) throws InterruptedException {
        long start = System.nanoTime();
        while ((System.nanoTime() - start) / 1_000_000 < timeoutMs) {
            if (cond.getAsBoolean()) return (System.nanoTime() - start) / 1_000_000;
            Thread.sleep(100);
        }
        return -1;
    }

    static long waitValid(long timeoutMs) throws InterruptedException {
        return waitFor(tanos::isValid, timeoutMs);
    }

    static String cmdline(long pid) {
        try {
            byte[] b = Files.readAllBytes(Paths.get("/proc/" + pid + "/cmdline"));
            return new String(b, StandardCharsets.UTF_8).replace('\0', ' ');
        } catch (Exception e) {
            return "";
        }
    }

    static long browserPid() {
        return ProcessHandle.current().children()
                .filter(p -> cmdline(p.pid()).contains("darkbot_browser"))
                .mapToLong(ProcessHandle::pid).findFirst().orElse(-1);
    }

    static long flashPid() {
        return ProcessHandle.current().descendants()
                .filter(p -> {
                    String c = cmdline(p.pid());
                    return c.contains("--type=ppapi") && !c.contains("ppapi-broker");
                })
                .mapToLong(ProcessHandle::pid).max().orElse(-1);
    }

    /** Open descriptors grouped by kind (socket, pipe, file path, ...), to locate leaks. */
    static String fdKinds() {
        java.util.Map<String, Integer> kinds = new java.util.TreeMap<>();
        try (java.util.stream.Stream<Path> fds = Files.list(Paths.get("/proc/self/fd"))) {
            fds.forEach(fd -> {
                try {
                    String target = Files.readSymbolicLink(fd).toString();
                    String kind = target.startsWith("/") ? target.replaceAll("[0-9]+", "N") : target.replaceAll(":\\[?[0-9]+\\]?", "");
                    kinds.merge(kind, 1, Integer::sum);
                } catch (Exception ignored) { }
            });
        } catch (Exception ignored) { }
        return kinds.toString();
    }

    static long selfStat(String what) {
        try {
            if (what.equals("fds")) {
                try (java.util.stream.Stream<Path> fds = Files.list(Paths.get("/proc/self/fd"))) {
                    return fds.count();
                }
            }
            for (String line : Files.readAllLines(Paths.get("/proc/self/status")))
                if (line.startsWith(what + ":")) return Long.parseLong(line.replaceAll("[^0-9]", ""));
        } catch (Exception ignored) { }
        return -1;
    }

    /**
     * Busy-bot simulation: concurrent API use from several threads, periodic reload and
     * flash crash injection; reports resource usage over time to spot leaks.
     */
    static void stress(String url, long seconds) throws Exception {
        tanos.setData(url, "dosid=integrationtest", "", "");
        new Thread(tanos::createWindow).start();
        result("launch_to_valid_ms", waitValid(90_000));

        java.util.concurrent.atomic.AtomicBoolean running = new java.util.concurrent.atomic.AtomicBoolean(true);
        java.util.concurrent.atomic.AtomicLong ops = new java.util.concurrent.atomic.AtomicLong();
        java.util.concurrent.atomic.AtomicReference<Throwable> error = new java.util.concurrent.atomic.AtomicReference<>();
        List<Thread> workers = new ArrayList<>();
        Runnable[] jobs = {
            () -> { long[] f = tanos.queryBytes(new byte[] {'F', 'W', 'S'}, 1); if (f.length > 0) for (int i = 0; i < 2000; i++) tanos.readLong(f[0] + i); },
            () -> { tanos.mouseMove(200, 200); tanos.mouseClick(300, 300); },
            () -> { tanos.callMethod(0x7f0000001000L, 10, 1, 2); tanos.checkMethodSignature(0x7f0000001000L, 10, true, "x"); },
            () -> { tanos.sendNotification(0x7f0000001000L, "n", 1, 2); tanos.useItem(0, "item", 19); tanos.keyClick('B'); },
            () -> { tanos.setSize(1200, 800); tanos.postActions((0x1FEL << 48) | ((long) 'C' << 32)); tanos.sendText("hi"); },
            () -> { tanos.getCpuUsage(); tanos.getMemoryUsage(); tanos.setVisible(true); },
        };
        for (Runnable job : jobs) {
            Thread t = new Thread(() -> {
                while (running.get()) {
                    try { job.run(); ops.incrementAndGet(); Thread.sleep(5); }
                    catch (InterruptedException e) { return; }
                    catch (Throwable e) { error.compareAndSet(null, e); }
                }
            });
            t.setDaemon(true);
            t.start();
            workers.add(t);
        }

        long start = System.currentTimeMillis(), nextEvent = start + 60_000;
        int event = 0, invalid = 0, checks = 0;
        long minute = start;
        while (System.currentTimeMillis() - start < seconds * 1000) {
            checks++;
            if (!tanos.isValid()) invalid++;
            if (System.currentTimeMillis() > nextEvent) {
                // alternate: bot refresh / flash crash
                if (event++ % 2 == 0) tanos.reload();
                else { long f = flashPid(); if (f > 0) ProcessHandle.of(f).ifPresent(ProcessHandle::destroyForcibly); }
                nextEvent = System.currentTimeMillis() + 60_000;
            }
            if (System.currentTimeMillis() - minute >= 60_000) {
                minute = System.currentTimeMillis();
                System.gc();
                System.out.printf("STAT t=%ds ops=%d rss_kb=%d fds=%d threads=%d invalid=%d/%d%n",
                        (minute - start) / 1000, ops.get(), selfStat("VmRSS"), selfStat("fds"), selfStat("Threads"), invalid, checks);
                System.out.println("FDS " + fdKinds());
            }
            Thread.sleep(100);
        }
        running.set(false);
        for (Thread t : workers) t.join(5000);
        result("stress_ops", ops.get());
        result("stress_events", event);
        result("stress_invalid_checks", invalid + "/" + checks);
        long settle = waitValid(90_000);
        result("stress_valid_at_end_ms", settle);
        check(error.get() == null, "no exceptions from JNI calls" + (error.get() != null ? ": " + error.get() : ""));
        check(settle >= 0, "valid at the end of the stress run");
    }

    /** Region statistics of a process: readable total, anonymous writable, file backed. */
    static void mapsSummary(long pid) {
        try {
            long readable = 0, anonRw = 0, fileRo = 0, fileRw = 0, biggest = 0;
            int regions = 0;
            for (String line : Files.readAllLines(Paths.get("/proc/" + pid + "/maps"))) {
                String[] f = line.trim().split("\\s+");
                String[] range = f[0].split("-");
                long size = Long.parseUnsignedLong(range[1], 16) - Long.parseUnsignedLong(range[0], 16);
                String perms = f[1];
                String name = f.length > 5 ? f[5] : "";
                regions++;
                if (perms.charAt(0) != 'r') continue;
                readable += size;
                biggest = Math.max(biggest, size);
                if (name.startsWith("/")) {
                    if (perms.charAt(1) == 'w') fileRw += size; else fileRo += size;
                } else if (perms.charAt(1) == 'w') anonRw += size;
            }
            result("flash_maps", String.format("regions=%d readable=%dMB anon_rw=%dMB file_ro=%dMB file_rw=%dMB biggest=%dMB",
                    regions, readable >> 20, anonRw >> 20, fileRo >> 20, fileRw >> 20, biggest >> 20));
        } catch (Exception e) {
            result("flash_maps", "error " + e);
        }
    }

    static double ms(long nanos) {
        return nanos / 1_000_000.0;
    }

    public static void main(String[] args) throws Exception {
        String url = args[0];
        byte[] swfHeader = Files.readAllBytes(Paths.get(args[1]));
        swfHeader = Arrays.copyOf(swfHeader, 64);

        if (Boolean.getBoolean("tanos.awt")) {
            // like DarkBot: a Swing GUI is running, AWT owns its own X connection/error handler
            javax.swing.SwingUtilities.invokeAndWait(() -> {
                javax.swing.JFrame frame = new javax.swing.JFrame("DarkBot GUI stand-in");
                frame.setSize(300, 200);
                frame.setVisible(true);
            });
            result("awt", "loaded");
        }

        result("api_version", tanos.getVersion());

        if (args.length > 3 && args[2].equals("--stress")) {
            stress(url, Long.parseLong(args[3]));
            finish();
        }

        if (args.length > 3 && args[2].equals("--soak")) {
            // keep the client running like the bot's main loop does, watch validity
            tanos.setData(url, "dosid=integrationtest", "", "");
            new Thread(tanos::createWindow).start();
            result("launch_to_valid_ms", waitValid(90_000));
            long end = System.currentTimeMillis() + Long.parseLong(args[3]) * 1000;
            int invalid = 0, checks = 0;
            while (System.currentTimeMillis() < end) {
                checks++;
                if (!tanos.isValid()) invalid++;
                Thread.sleep(100);
            }
            result("soak_invalid_checks", invalid + "/" + checks);
            mapsSummary(flashPid());
            byte[] absent = {(byte) 0x9a, 0x13, 0x77, (byte) 0xe1, 0x55, 0x02, (byte) 0xc4, 0x3b, 0x61, (byte) 0xd8, 0x0f, 0x2e};
            for (int i = 0; i < 3; i++) {
                long q = System.nanoTime();
                long[] r = tanos.queryBytes(absent, 1);
                result("full_scan_ms_" + i, String.format("%.1f (found %d)", ms(System.nanoTime() - q), r.length));
            }
            finish();
        }

        if (args.length > 2 && args[2].equals("--launch-and-wait")) {
            // used to test cleanup when the bot is killed (-9)
            tanos.setData(url, "dosid=integrationtest", "", "");
            new Thread(tanos::createWindow).start();
            System.out.println("RESULT launch_only_valid_ms " + waitValid(90_000));
            Thread.sleep(Long.MAX_VALUE);
        }

        // --- launch ---
        tanos.setData(url, "dosid=integrationtest", "", "");
        long t0 = System.nanoTime();
        Thread api = new Thread(tanos::createWindow, "API thread");
        api.setDaemon(true);
        api.start();
        long launchMs = waitValid(90_000);
        result("launch_to_valid_ms", launchMs);
        check(launchMs >= 0, "browser launched and flash process found");
        if (launchMs < 0) {
            finish();
            return;
        }
        Thread.sleep(3000); // let the SWF run (JIT + GC activity)

        // --- Java child processes keep working (no stolen SIGCHLD / exit codes) ---
        int wrongExit = 0;
        for (int i = 0; i < 20; i++) {
            Process p = new ProcessBuilder("sh", "-c", "exit 7").start();
            if (p.waitFor() != 7) wrongExit++;
        }
        result("java_child_wrong_exit_codes", wrongExit + "/20");
        check(wrongExit == 0, "Process.waitFor() exit codes intact after browser launch");

        // --- memory access in the flash process ---
        long q0 = System.nanoTime();
        long[] found = tanos.queryBytes(Arrays.copyOf(swfHeader, 24), 4);
        result("query_bytes_ms", String.format("%.1f", ms(System.nanoTime() - q0)));
        result("query_bytes_found", found.length);
        check(found.length > 0, "queryBytes finds the SWF in flash memory");
        if (found.length > 0) {
            byte[] read = tanos.readBytes(found[0], 24);
            check(Arrays.equals(read, Arrays.copyOf(swfHeader, 24)), "readBytes returns the same bytes");
            byte[] buf = new byte[64];
            tanos.readBytes(found[0], buf, 8);
            check(buf[0] == 'F' && buf[8] == 0, "readBytes(buf, len) reads only len bytes");
            try {
                tanos.readBytes(found[0], new byte[4], 16);
                check(true, "readBytes(buf, len > buf.length) doesn't throw");
            } catch (Throwable e) {
                check(false, "readBytes(buf, len > buf.length) doesn't throw: " + e);
            }

            int n = 200_000;
            long r0 = System.nanoTime();
            long sum = 0;
            for (int i = 0; i < n; i++) sum += tanos.readInt(found[0] + (i & 7));
            result("read_int_ns", String.format("%.0f", (System.nanoTime() - r0) / (double) n));
        }
        check(tanos.readLong(0x10) == 0, "reading an invalid address returns 0");

        // --- direct game calls while do_lib's game hooks are not installed (no game loaded) ---
        int sigGarbage = 0;
        long c0 = System.nanoTime();
        for (int i = 0; i < 3; i++) {
            int r = tanos.checkMethodSignature(0x7f0000001000L, 10, false, "26(267726?2?)42407911700");
            if (r == 0 || r > 1) sigGarbage++; // 0 would throw InvalidNativeSignature in the bot
        }
        result("check_signature_avg_ms", String.format("%.1f", ms(System.nanoTime() - c0) / 3));
        result("check_signature_bad_results", sigGarbage + "/3");
        check(sigGarbage == 0, "checkMethodSignature never reports 'invalid' on transport failure");

        long m0 = System.nanoTime();
        for (int i = 0; i < 3; i++) tanos.callMethod(0x7f0000001000L, 10, 1, 2);
        result("call_method_avg_ms", String.format("%.1f", ms(System.nanoTime() - m0) / 3));

        long k0 = System.nanoTime();
        for (int i = 0; i < 3; i++) tanos.keyClick('A');
        result("key_click_legacy_avg_ms", String.format("%.1f", ms(System.nanoTime() - k0) / 3));

        long s0 = System.nanoTime();
        for (int i = 0; i < 3; i++) tanos.sendNotification(0x7f0000001000L, "MapAssetNotificationTRY_TO_SELECT_MAPASSET", 1, 2, 3);
        result("select_entity_avg_ms", String.format("%.1f", ms(System.nanoTime() - s0) / 3));

        // --- browser commands (unix socket) ---
        long b0 = System.nanoTime();
        for (int i = 0; i < 20; i++) tanos.setSize(1000 + i, 700);
        result("browser_command_avg_ms", String.format("%.2f", ms(System.nanoTime() - b0) / 20));

        long keyAction = (0x1FEL << 48) | ((long) 'Q' << 32);
        long p0 = System.nanoTime();
        tanos.postActions(keyAction);
        result("post_action_key_ms", String.format("%.2f", ms(System.nanoTime() - p0)));

        // --- X11 input ---
        long x0 = System.nanoTime();
        for (int i = 0; i < 200; i++) tanos.mouseMove(100 + i, 100 + i);
        result("mouse_move_avg_ms", String.format("%.3f", ms(System.nanoTime() - x0) / 200));
        long x1 = System.nanoTime();
        for (int i = 0; i < 50; i++) tanos.mouseClick(300, 300);
        result("mouse_click_avg_ms", String.format("%.3f", ms(System.nanoTime() - x1) / 50));

        result("cpu_usage", String.format("%.1f", tanos.getCpuUsage()));
        result("memory_usage_mb", tanos.getMemoryUsage());

        // --- health check & recovery ---
        long oldFlash = flashPid();
        long f0 = System.nanoTime();
        tanos.reload();
        long reloadMs = waitFor(() -> tanos.isValid() && flashPid() > 0 && flashPid() != oldFlash, 90_000);
        result("reload_to_valid_ms", reloadMs >= 0 ? (System.nanoTime() - f0) / 1_000_000 : -1);
        check(reloadMs >= 0, "reload(): valid again with a new flash process");

        long flash = flashPid();
        if (flash > 0) {
            ProcessHandle.of(flash).ifPresent(ProcessHandle::destroyForcibly); // SIGKILL
            long k = waitFor(() -> tanos.isValid() && flashPid() > 0 && flashPid() != flash, 90_000);
            result("flash_crash_to_valid_ms", k);
            check(k >= 0, "flash process crash: recovered");
        }

        long browser = browserPid();
        if (browser > 0) {
            ProcessHandle.of(browser).ifPresent(ProcessHandle::destroyForcibly); // SIGKILL
            long k = waitFor(() -> tanos.isValid() && browserPid() > 0 && browserPid() != browser, 120_000);
            result("browser_crash_to_valid_ms", k);
            check(k >= 0, "browser process crash: recovered");
        }

        long zombies = ProcessHandle.current().children()
                .filter(p -> {
                    try {
                        String stat = new String(Files.readAllBytes(Paths.get("/proc/" + p.pid() + "/stat")));
                        return stat.substring(stat.lastIndexOf(')') + 2).startsWith("Z");
                    } catch (Exception e) {
                        return false;
                    }
                }).count();
        result("zombie_children", zombies);
        check(zombies == 0, "no zombie browser processes");

        finish();
    }

    static void finish() {
        System.out.println(failures.isEmpty() ? "ALL CHECKS PASSED" : "FAILED CHECKS: " + failures);
        System.exit(failures.isEmpty() ? 0 : 1);
    }
}
