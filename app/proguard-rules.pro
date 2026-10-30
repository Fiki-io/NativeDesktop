# Native JNI methods must be preserved
-keepclasseswithmembernames class * {
    native <methods>;
}

-keep class com.core.desktop.data.repository.DesktopEngineRepository {
    *;
}

