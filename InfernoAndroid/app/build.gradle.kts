import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.net.URI
import java.security.MessageDigest
import java.util.zip.ZipFile

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Where engine/build-engine.sh left the engine: libqemu-aarch64-softmmu.so and
// qemu-bundle/. -PengineDir=/path overrides it. Without an engine the APK still
// builds; the app then says so instead of starting.
val engineDir = file(providers.gradleProperty("engineDir").orElse(rootProject.file("build/engine").absolutePath).get())
val engineLib = File(engineDir, "libqemu-aarch64-softmmu.so")
val engineBundled = engineLib.isFile
val systemSigner = file(providers.gradleProperty("systemCodeSigner")
    .orElse(rootProject.file("build/system-codesign/libember_codesign.so").absolutePath).get())
val systemSignerBundled = systemSigner.isFile

android {
    namespace = "dev.embervm.android"
    compileSdk = 35
    buildToolsVersion = "35.0.0"
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "dev.embervm.android"
        minSdk = 29
        targetSdk = 35
        versionCode = 17
        versionName = "0.7.0-gpubroker"
        ndk { abiFilters += "arm64-v8a" }
        buildConfigField("boolean", "ENGINE_BUNDLED", engineBundled.toString())
        buildConfigField("boolean", "SYSTEM_SIGNER_BUNDLED", systemSignerBundled.toString())
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            // Signed with the debug key so a release build installs straight away;
            // swap in your own signingConfig to publish it.
            signingConfig = signingConfigs.getByName("debug")
        }
        // Installs beside a working Ember VM (dev.embervm.android) instead of
        // over it: another package, its own Android/data folder and debug key.
        // Import the prepared iOS files with tools/import-from-installed.sh.
        create("gpuTest") {
            initWith(getByName("debug"))
            applicationIdSuffix = ".gpu"
            versionNameSuffix = "-gputest"
            matchingFallbacks += "debug"
        }
    }

    buildFeatures { compose = true; buildConfig = true }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    // Uncompressed and page-aligned in the APK, so the loader maps the engine
    // straight out of it instead of unpacking 100 MB on install.
    packaging { jniLibs { useLegacyPackaging = false } }
    testOptions { unitTests.isReturnDefaultValues = true }
}

dependencies {
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.compose.ui:ui:1.7.8")
    implementation("androidx.compose.foundation:foundation:1.7.8")
    implementation("androidx.compose.material3:material3:1.3.2")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("net.sf.kxml:kxml2:2.3.0")
}

// The engine goes in as a prebuilt JNI library, its data files as assets.
val engineJniLibs = layout.buildDirectory.dir("generated/engine/jniLibs")
val engineAssets = layout.buildDirectory.dir("generated/engine/assets")
val stageEngine = tasks.register<Sync>("stageEngine") {
    into(layout.buildDirectory.dir("generated/engine"))
    from(rootProject.file("LICENSE")) { into("assets/licenses"); rename { "EmberVM-GPL-3.0.txt" } }
    if (engineBundled) {
        from(engineLib) { into("jniLibs/arm64-v8a") }
        from(File(engineDir, "qemu-bundle/usr/local/share/qemu")) { into("assets/qemu-data") }
    }
}
android.sourceSets["main"].jniLibs.srcDir(engineJniLibs)
android.sourceSets["main"].assets.srcDir(engineAssets)
tasks.named("preBuild").configure { dependsOn(stageEngine) }

// Optional development dependency for preparing the copied system executable.
// Compiling it alone does not enable the unfinished system GPU frontend.
val signerOutput=layout.buildDirectory.dir("generated/system-signer")
val stageSystemSigner=tasks.register<Sync>("stageSystemSigner") {
    into(signerOutput)
    // Always give Sync a source so an unbundled build removes a previously
    // staged library instead of being skipped as NO-SOURCE.
    from(rootProject.file("gpu-bridge/system/licenses")) { into("assets/licenses/system-signer") }
    if(systemSignerBundled) {
        from(systemSigner) { into("jniLibs/arm64-v8a") }
    }
    doFirst { if(systemSignerBundled) {
        check(systemSigner.length() in 20..16L*1024*1024)
        val h=systemSigner.inputStream().use { it.readNBytes(20) }
        check(h[0]==0x7f.toByte() && h[1]=='E'.code.toByte() && h[2]=='L'.code.toByte() && h[3]=='F'.code.toByte() &&
            h[4]==2.toByte() && h[5]==1.toByte() && h[16]==3.toByte() && h[17]==0.toByte() && h[18]==0xb7.toByte() && h[19]==0.toByte()) {
            "System code signer must be an Android arm64 ELF shared library"
        }
    } }
}
android.sourceSets["main"].jniLibs.srcDir(signerOutput.map { it.dir("jniLibs") })
android.sourceSets["main"].assets.srcDir(signerOutput.map { it.dir("assets") })
tasks.named("preBuild").configure { dependsOn(stageSystemSigner) }

// Only the two redistributable guest programs, never Apple firmware.
val guestAssets = layout.buildDirectory.dir("generated/guest-assets")
val stageGuestTools = tasks.register("stageGuestTools") {
    val cache = layout.buildDirectory.dir("guest-tools")
    outputs.dir(guestAssets)
    // A missing download may be retried next build; never retain stale tools.
    outputs.upToDateWhen { false }
    doLast {
        val out = guestAssets.get().asFile.apply { deleteRecursively(); mkdirs() }
        val ipa = File(cache.get().asFile, "Inferno-v0.3.1.ipa")
        val hash = "c66b56de80d55b989df1c70ebcf4b321751a6703a9d253c9ff349f434fd7c8dc"
        if (!ipa.isFile) {
            ipa.parentFile.mkdirs()
            val part = File(ipa.path + ".part")
            try {
                val connection = URI("https://github.com/MakrSas/Inferno-iOS/releases/download/v0.3.1/Inferno.ipa").toURL().openConnection()
                connection.connectTimeout = 30000; connection.readTimeout = 30000
                connection.getInputStream().use { input -> part.outputStream().use { output ->
                    val b = ByteArray(65536); var total = 0L
                    while (true) { val n = input.read(b); if (n < 0) break; total += n; check(total <= 64L*1024*1024); output.write(b,0,n) }
                } }
                Files.move(part.toPath(), ipa.toPath(), StandardCopyOption.REPLACE_EXISTING)
            } catch (e: Exception) {
                logger.warn("Guest tools unavailable: ${e.message}. Restore will require filesystem patches.")
                return@doLast
            } finally { part.delete() }
        }
        val digest = MessageDigest.getInstance("SHA-256")
        ipa.inputStream().use { input -> val b = ByteArray(65536); while (true) { val n=input.read(b);if(n<0)break;digest.update(b,0,n) } }
        check(digest.digest().joinToString("") { "%02x".format(it) } == hash) { "Guest IPA SHA-256 mismatch; delete the cached IPA and retry" }
        ZipFile(ipa).use { zip ->
            for (name in listOf("inferno_patcher", "inferno_fs_patcher")) {
                val entry = zip.getEntry("Payload/Inferno.app/guest/$name") ?: error("Missing guest program $name")
                check(entry.size in 1..(16L*1024*1024))
                val target = File(out, "guest/$name").apply { parentFile.mkdirs() }
                zip.getInputStream(entry).use { input -> target.outputStream().use { input.copyTo(it) } }
            }
        }
        val daemon = rootProject.file("guest/bin/inferno_patcher")
        check(daemon.isFile) { "Build the GPL guest daemon with guest/build.sh" }
        val daemonHash = "13d49f8cf9962fbbbec243fb365bca3a14541ad49eaf8c13bf367bcd6971114b"
        check(MessageDigest.getInstance("SHA-256").digest(daemon.readBytes()).joinToString("") { "%02x".format(it) } == daemonHash) { "Guest daemon SHA-256 mismatch" }
        daemon.copyTo(File(out,"guest/inferno_patcher"),overwrite=true)
        File(out, "guest/provenance.txt").writeText("Ember patch-only daemon 0.3.1, GPL-3.0-or-later; source guest/patcher-daemon.c\nSHA-256 $daemonHash\n" + "MakrSas/Inferno-iOS v0.3.1\nIPA SHA-256 $hash\nGPL-3.0-or-later; sources: https://github.com/MakrSas/Inferno-iOS/tree/v0.3.1/app/guest\n")
    }
}
android.sourceSets["main"].assets.srcDir(guestAssets)
tasks.named("preBuild").configure { dependsOn(stageGuestTools) }

// Only our GPL diagnostic code; never an Apple ramdisk, ROM or firmware.
val gpuAssets=layout.buildDirectory.dir("generated/gpu-assets")
val stageGpuTools=tasks.register<Sync>("stageGpuTools") {
    val binaries=mapOf(
        "eagl-present-probe-ios" to "6bce64857fee456d839e66b6d047357170d96f4b4726868c421f22c8a10da10b",
        "eagl-api-probe-ios" to "d70d98b480220a0ac2d40ad2255ce9d8ccc372a427da891e4e30cd4a0a8d9e74",
        "libEmberGLES.dylib" to "a2baaf7d15cd9bf11ec459b83673ea902d753ba4d72f8e15db8bca2d1f9eb753",
        "broker-launcher-ios" to "f8e40067e34282e0013e15dc85893190240e2ccb5bafc33f82b46fd858c0ed8d",
        "broker-client-ios" to "8937514de73dcb996db1e00c334f6a8aa7ce5e5616dc67a790e6b6088be1f81d",
    )
    from(rootProject.file("gpu-bridge/bin")) { include(binaries.keys);into("gpu") }
    into(gpuAssets)
    inputs.property("expectedGpuHashes",binaries)
    doFirst {
        for((name,hash) in binaries) {
            val file=rootProject.file("gpu-bridge/bin/$name")
            check(MessageDigest.getInstance("SHA-256").digest(file.readBytes()).joinToString("") { "%02x".format(it) }==hash) {
                "GPU helper hash mismatch: $name; rebuild and update provenance"
            }
        }
    }
}
android.sourceSets["main"].assets.srcDir(gpuAssets)
tasks.named("preBuild").configure { dependsOn(stageGpuTools) }

// Keep large sparse-copy fixtures off a disk-constrained workspace without
// JAVA_TOOL_OPTIONS leaking startup text into cross-process lock fixtures.
tasks.withType<Test>().configureEach {
    providers.gradleProperty("testTmpDir").orNull?.let { systemProperty("java.io.tmpdir",file(it).absolutePath) }
    providers.gradleProperty("systemGpuPeer").orNull?.let { systemProperty("ember.systemGpuPeer",file(it).absolutePath) }
    providers.gradleProperty("systemGpuNativeClient").orNull?.let { systemProperty("ember.systemGpuNativeClient",file(it).absolutePath) }
    providers.gradleProperty("systemGpuNativeDir").orNull?.let {
        systemProperty("java.library.path",file(it).absolutePath)
        systemProperty("ember.systemGpuNativeTest","true")
    }
    providers.gradleProperty("systemGpuGlPrefix").orNull?.let {
        environment("LD_LIBRARY_PATH",File(file(it),"lib/x86_64-linux-gnu").absolutePath)
        environment("EGL_PLATFORM","surfaceless")
        environment("LIBGL_ALWAYS_SOFTWARE","1")
    }
}
