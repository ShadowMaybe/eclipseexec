# Rules for building this module itself. It is not minified — minifyEnabled
# is false in build.gradle — so these only matter if that ever changes.
#
# The rules that actually affect consumers live in consumer-rules.pro and are
# packaged into the AAR, where R8 will see them.
-keep class me.shadow.eclipselauncher.exec.EclipseExec { *; }
