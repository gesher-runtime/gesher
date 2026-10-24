package gesher;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A CPU-bound benchmark for ART's execution modes (docs/compiled-code.md):
 * the same work under the switch interpreter, nterp, AOT and JIT code.
 *
 *   gesher.Bench [rounds]
 *
 * Prints one line per kernel, "bench <kernel> <ms>", the best of the rounds,
 * then "bench total <ms>" and "Bench done checksum=<n>"; the checksum is the
 * same in every mode (tests compare it).
 */
public final class Bench {
  interface Shape {
    double area();
  }

  static final class Square implements Shape {
    final double side;

    Square(double side) {
      this.side = side;
    }

    public double area() {
      return side * side;
    }
  }

  static final class Circle implements Shape {
    final double radius;

    Circle(double radius) {
      this.radius = radius;
    }

    public double area() {
      return 3.14159 * radius * radius;
    }
  }

  static long checksum;

  static long fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
  }

  static long intLoops() {
    long sum = 0;
    for (int i = 0; i < 3_000_000; i++) {
      sum += (i * 31) ^ (i >>> 3);
      if ((i & 7) == 0) sum -= i / 3;
    }
    return sum;
  }

  static long sieve() {
    boolean[] composite = new boolean[400_000];
    long count = 0;
    for (int i = 2; i < composite.length; i++) {
      if (!composite[i]) {
        count++;
        for (int j = i * 2; j < composite.length; j += i) composite[j] = true;
      }
    }
    return count;
  }

  static long sort() {
    int[] a = new int[100_000];
    int seed = 12345;
    for (int i = 0; i < a.length; i++) {
      seed = seed * 1103515245 + 12345;
      a[i] = seed >>> 1;
    }
    // Insertion sort over chunks plus a library sort: loops and calls.
    for (int start = 0; start + 64 <= a.length; start += 64) {
      for (int i = start + 1; i < start + 64; i++) {
        int v = a[i];
        int j = i - 1;
        while (j >= start && a[j] > v) {
          a[j + 1] = a[j];
          j--;
        }
        a[j + 1] = v;
      }
    }
    Arrays.sort(a);
    return a[a.length / 2];
  }

  static long virtualCalls() {
    Shape[] shapes = new Shape[64];
    for (int i = 0; i < shapes.length; i++) {
      shapes[i] = (i & 1) == 0 ? new Square(i) : new Circle(i);
    }
    double total = 0;
    for (int round = 0; round < 20_000; round++) {
      for (Shape s : shapes) total += s.area();
    }
    return (long) total;
  }

  static long allocation() {
    long sum = 0;
    for (int round = 0; round < 50; round++) {
      List<Integer> list = new ArrayList<>();
      for (int i = 0; i < 10_000; i++) list.add(i);
      Map<Integer, String> map = new HashMap<>();
      for (int i = 0; i < 2_000; i++) map.put(i, "v" + i);
      sum += list.size() + map.get(round).length();
    }
    return sum;
  }

  static long strings() {
    StringBuilder sb = new StringBuilder();
    long sum = 0;
    for (int i = 0; i < 20_000; i++) {
      sb.setLength(0);
      sb.append("item").append(i).append(':').append(i * 7);
      String s = sb.toString();
      sum += s.hashCode() + s.indexOf(':');
    }
    return sum;
  }

  static long floats() {
    double x = 0;
    for (int i = 1; i < 300_000; i++) {
      x += Math.sqrt(i) * 0.5 + Math.sin(i * 0.001) + (double) i / (i + 1);
    }
    return (long) x;
  }

  interface Kernel {
    long run();
  }

  static long time(String name, Kernel k, int rounds) {
    long best = Long.MAX_VALUE;
    long result = 0;
    for (int r = 0; r < rounds; r++) {
      long t0 = System.nanoTime();
      result = k.run();
      long ms = (System.nanoTime() - t0) / 1_000_000;
      best = Math.min(best, ms);
    }
    checksum = checksum * 31 + result;
    System.out.println("bench " + name + " " + best);
    return best;
  }

  public static void main(String[] args) {
    int rounds = args.length > 0 ? Integer.parseInt(args[0]) : 3;
    long total = 0;
    total += time("fib", () -> fib(25), rounds);
    total += time("int-loops", Bench::intLoops, rounds);
    total += time("sieve", Bench::sieve, rounds);
    total += time("sort", Bench::sort, rounds);
    total += time("virtual-calls", Bench::virtualCalls, rounds);
    total += time("allocation", Bench::allocation, rounds);
    total += time("strings", Bench::strings, rounds);
    total += time("floats", Bench::floats, rounds);
    System.out.println("bench total " + total);
    System.out.println("Bench done checksum=" + checksum);
  }
}
