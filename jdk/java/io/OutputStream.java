package java.io;
public class OutputStream {
    public OutputStream() {
    }
    public void write(char c) {
        write((int)c);
    }
    public void write(int b) {
        PrintStream.nativeWrite(b);
    }
    protected static native int nativeWrite(int b);
    public static void put(int b) {
        OutputStream.nativeWrite(b);
    }
    public void flush() {
    }
}
