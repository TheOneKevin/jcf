package com.example;

public class HelloWorld extends Object implements Runnable, Serializable {
  public HelloWorld() {}
  public int test = another.object().method();
  public int nullField = null;
  public static void main(String[] args) {
    int x = 0;
    for(int i = 0; i < args.length; i=i+1) {
      x = x + Integer.parseInt(args[i]);
    }
    for(int i = 0; i < 10; i = i+1) {
      if(true)
        System.out.println("Hello, World");
      else
        System.out.println("Goodbye, World");
    }
  }
}
