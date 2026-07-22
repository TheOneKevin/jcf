// Base class for the polymorphism test. speak() is overridden by Dog; a call
// through an Animal reference must dispatch to the runtime type's override.
public class Animal {
    public Animal() {}
    public int speak() {
        return 1;
    }
}
