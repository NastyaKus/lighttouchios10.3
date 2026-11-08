#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run current synthetic USB pressure tests against 0.2.4's receive scheduler.

First run :app:testDebugUnitTest on the updated tree. Requires a JDK and cached
Kotlin 2.1.20/JUnit dependencies (GRADLE_USER_HOME). No Android/Apple device data.
The old code receives only the clock/constructor adapter needed by these tests.
Without git history, set EMBER_BASELINE_GUEST_USB to the baseline .kt file
included in the update archive's validation directory.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
cache = Path(os.environ.get("GRADLE_USER_HOME", str(Path.home() / ".gradle"))) / "caches/modules-2/files-2.1"
jdk = Path(os.environ["JAVA_HOME"]) / "bin"


def jar(group, name, version):
    files = list((cache / group / name / version).glob("*/*.jar"))
    if len(files) != 1:
        raise RuntimeError(f"Expected one cached jar for {group}:{name}:{version}")
    return str(files[0])


compiler = [jar(*d) for d in [
    ("org.jetbrains.kotlin", "kotlin-compiler-embeddable", "2.1.20"),
    ("org.jetbrains.kotlin", "kotlin-stdlib", "2.1.20"),
    ("org.jetbrains.kotlin", "kotlin-script-runtime", "2.1.20"),
    ("org.jetbrains.kotlin", "kotlin-reflect", "1.6.10"),
    ("org.jetbrains.intellij.deps", "trove4j", "1.0.20200330"),
    ("org.jetbrains.kotlinx", "kotlinx-coroutines-core-jvm", "1.8.0"),
    ("org.jetbrains", "annotations", "13.0"),
]]
junit = jar("junit", "junit", "4.13.2")
hamcrest = jar("org.hamcrest", "hamcrest-core", "1.3")
baseline_ref = "0aaa1f4"
if os.environ.get("EMBER_BASELINE_GUEST_USB"):
    baseline_ref = "provided source file (see SHA-256)"
    baseline = Path(os.environ["EMBER_BASELINE_GUEST_USB"]).read_text()
else:
    baseline = subprocess.check_output([
        "git", "show", baseline_ref + ":InfernoAndroid/app/src/main/java/dev/embervm/android/restore/GuestUsb.kt"
    ], cwd=root).decode()
signature = "class GuestUsb(private val link:UsbLink, private val note:(String)->Unit={}):GuestConnector,Closeable {"
assert baseline.count(signature) == 1
adapted = baseline.replace(signature,
    "class GuestUsb(private val link:UsbLink, private val clock:()->Long={System.nanoTime()/1_000_000}, "
    "private val note:(String)->Unit={}):GuestConnector,Closeable {\n    private fun now()=clock()")
static_clock = "        private fun now()=System.nanoTime()/1_000_000\n"
assert adapted.count(static_clock) == 1
adapted = adapted.replace(static_clock, "")

methods = [
    "incomingEndpointIsServicedBeforeWritableTcpWindowFills",
    "busyGuestThatAlreadyAcceptedMuxDataDoesNotTriggerBlindReplay",
    "lostMuxFrameIsReplayedWithoutFloodingGuestOrReplayingAcceptedPrefix",
]
runner = '''
import org.junit.runner.JUnitCore;
import org.junit.runner.Request;
public class BaselinePressure {
  public static void main(String[] methods) throws Exception {
    Class<?> tests = Class.forName("dev.embervm.android.restore.ProtocolTest");
    for (String method : methods) {
      var result = new JUnitCore().run(Request.method(tests, method));
      if (result.wasSuccessful() || result.getFailureCount() != 1)
        throw new AssertionError("Baseline did not reproduce failure: " + method);
      var cause = result.getFailures().get(0).getException();
      boolean queueOverflow = cause instanceof IllegalStateException &&
          "Fake guest IN endpoint queue overflow".equals(cause.getMessage());
      boolean replayedAcceptedData = method.startsWith("busyGuest") &&
          cause instanceof AssertionError && cause.getMessage() != null &&
          cause.getMessage().startsWith("expected:<256> but was:<");
      if (!queueOverflow && !replayedAcceptedData)
        throw new AssertionError("Unexpected baseline failure", cause);
      System.out.println(method + ": reproduced " + cause.getMessage());
    }
  }
}
'''
with tempfile.TemporaryDirectory(prefix="ember-usb-baseline-") as temporary:
    work = Path(temporary)
    (work / "GuestUsb.kt").write_text(adapted)
    classes = work / "classes"
    subprocess.run([str(jdk / "java"), "-Xmx768m", "-cp", os.pathsep.join(compiler),
        "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect",
        "-classpath", compiler[1], "-module-name", "app_debug", "-jvm-target", "17",
        "-d", str(classes), str(work / "GuestUsb.kt")], check=True)
    (work / "BaselinePressure.java").write_text(runner)
    cp = os.pathsep.join([str(classes), junit, hamcrest, compiler[1],
        str(root / "app/build/tmp/kotlin-classes/debugUnitTest"),
        str(root / "app/build/tmp/kotlin-classes/debug")])
    subprocess.run([str(jdk / "javac"), "-cp", cp, "-d", str(classes),
        str(work / "BaselinePressure.java")], check=True)
    result = subprocess.run([str(jdk / "java"), "-cp", cp, "BaselinePressure", *methods],
        text=True, capture_output=True)
    print(result.stdout, end="")
    if result.returncode:
        print(result.stderr, end="")
        result.check_returncode()
    report = {"baseline": baseline_ref, "baseline_sha256": hashlib.sha256(baseline.encode()).hexdigest(),
        "adapter": "clock and constructor only; no scheduler/protocol changes",
        "expected_failures": ["Fake guest IN endpoint queue overflow", "Already accepted data transmitted again during busy-guest wait"], "methods": methods,
        "reproduced_failures": len(methods), "apple_inputs": False,
        "output": result.stdout.splitlines(),
        "updated_results": "See app/build/test-results/testDebugUnitTest and validation-results.json"}
    output = root / "build/usb-replay-regression"
    output.mkdir(parents=True, exist_ok=True)
    (output / "baseline-results.json").write_text(json.dumps(report, indent=2) + "\n")
