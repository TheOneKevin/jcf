// The real goal: print "Hello, World!" through the unmodified JDK, using
// System.out.println with a string literal. This exercises the full
// object-oriented path: string-literal construction (char[] + java.lang.String),
// the static field System.out (a PrintStream initialized by jcf.static.init),
// virtual dispatch of println/print/write, instance field access (String.chars),
// and the native OutputStream.nativeWrite backing the write.
public class HelloJDK {
    public HelloJDK() {}

    public static int test() {
        System.out.println("Hello, World!");
        return 0;
    }
}
