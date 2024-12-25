## Linux Build
```
sudo apt-get install libsndfile1-dev
g++ -std=c++17 src/main.cpp -o slicer -I./src -lsamplerate -lsndfile
```

## Running
```
mkdir res
./slicer -i /path/to/audio/file -s res 
```
