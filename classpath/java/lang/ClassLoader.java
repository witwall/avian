package java.lang;

public class ClassLoader {
  private static final ClassLoader instance = new ClassLoader();

  private ClassLoader() { }

  public static ClassLoader getSystemClassLoader() {
    return instance;
  }

  public Class loadClass(String name) {
    return Class.forName(name);
  }
}