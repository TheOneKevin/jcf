// Subclass overriding speak(). Also stores a field set by its constructor, so
// the test exercises instance fields and constructor super()-chaining too.
public class Dog extends Animal {
    public int noise;
    public Dog() {
        this.noise = 42;
    }
    public int speak() {
        return this.noise;
    }
}
