package android.os;

/** Stand-in for the framework interface. */
public interface Parcelable {
  int describeContents();

  void writeToParcel(Parcel dest, int flags);

  interface Creator<T> {
    T createFromParcel(Parcel source);

    T[] newArray(int size);
  }
}
