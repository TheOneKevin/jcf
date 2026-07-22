// Array-free engine test for tir-vm: exercises local variables (alloca /
// load / store), a loop (branch + compare), and integer arithmetic. Returns
// 1 + 2 + 3 + 4 + 5 = 15, which becomes the process exit code.
public class Sum {
    public Sum() {}

    public static int test() {
        int sum = 0;
        for (int i = 1; i <= 5; i = i + 1) {
            sum = sum + i;
        }
        return sum;
    }
}
