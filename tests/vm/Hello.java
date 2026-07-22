// A "hello world" that works within the current codegen limitations: it uses
// only static method calls with primitive arguments. Object construction,
// string literals, and instance methods are not yet emitted by the front-end,
// so we print the message one character at a time through
// java.io.OutputStream.put, which forwards to the native nativeWrite that
// tir-vm implements.
//
// NOTE: we intentionally avoid arrays here. The front-end currently miscompiles
// array element access (a dangling-initializer_list bug in utils::range_ref
// makes getelementptr emit index 0 for the array's data-pointer field), so an
// int[] message buffer would be corrupted. See Sum.java for an array-free
// engine test and the tir-vm notes for details.
import java.io.OutputStream;

public class Hello {
    public Hello() {}

    public static int test() {
        OutputStream.put(72);   // H
        OutputStream.put(101);  // e
        OutputStream.put(108);  // l
        OutputStream.put(108);  // l
        OutputStream.put(111);  // o
        OutputStream.put(44);   // ,
        OutputStream.put(32);   // (space)
        OutputStream.put(87);   // W
        OutputStream.put(111);  // o
        OutputStream.put(114);  // r
        OutputStream.put(108);  // l
        OutputStream.put(100);  // d
        OutputStream.put(33);   // !
        OutputStream.put(10);   // \n
        return 0;
    }
}
