// Polymorphism test: bind a Dog to an Animal reference and call speak(). Virtual
// dispatch must select Dog.speak() (which returns the field noise = 42), not
// Animal.speak() (which returns 1). The result becomes the process exit code.
public class Poly {
    public Poly() {}
    public static int test() {
        Animal a = new Dog();
        return a.speak();
    }
}
