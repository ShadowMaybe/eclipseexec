# Rules that ship inside the AAR and apply to every app that includes it.
#
# EclipseExec is bound from native code in JNI_OnLoad() with FindClass() and
# RegisterNatives(). R8 can see neither call, so without this rule it renames
# or strips the class and System.loadLibrary() fails with
# "class me.shadow.eclipselauncher.exec.EclipseExec not found" — in a release
# build, on a user's device, with no way to recover.
-keep class me.shadow.eclipselauncher.exec.EclipseExec { *; }

# Belt and braces: any class with native methods keeps its name, which is what
# a hand-written JNI_OnLoad or an unbundled System.loadLibrary call would need.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
