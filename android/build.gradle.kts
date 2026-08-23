allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

// Keep the :app / Flutter output under <project>/build (same as Flutter CLI).
val newBuildDir: Directory =
    rootProject.layout.buildDirectory
        .dir("../../build")
        .get()
rootProject.layout.buildDirectory.value(newBuildDir)

// Plugin modules keep their default build directories. Redirecting them into
// this project's build/ breaks Gradle when the pub cache lives on another drive
// ("this and base files have different roots") and Android Studio loses Run.
subprojects {
    if (name == "app") {
        project.layout.buildDirectory.value(newBuildDir.dir(project.name))
    }
}

// Force plugins (e.g. permission_handler) onto API 37.0 as well. Integer
// compileSdk 37 resolves to a missing platforms/android-37 directory.
subprojects {
    afterEvaluate {
        extensions.findByName("android")?.let { androidExt ->
            try {
                androidExt.javaClass
                    .getMethod("setCompileSdkVersion", String::class.java)
                    .invoke(androidExt, "android-37.0")
            } catch (_: Throwable) {
                try {
                    androidExt.javaClass
                        .getMethod("setCompileSdk", Int::class.javaPrimitiveType)
                        .invoke(androidExt, 37)
                } catch (_: Throwable) {
                    // Ignore plugins without a compatible DSL setter.
                }
            }
        }
    }
}

subprojects {
    project.evaluationDependsOn(":app")
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
