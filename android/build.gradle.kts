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

subprojects {
    afterEvaluate {
        extensions.findByName("android")?.let { androidExt ->
            try {
                androidExt.javaClass
                    .getMethod("setCompileSdk", Int::class.javaPrimitiveType)
                    .invoke(androidExt, 37)
            } catch (_: Throwable) {
                try {
                    androidExt.javaClass
                        .getMethod("setCompileSdkVersion", Int::class.javaPrimitiveType)
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
