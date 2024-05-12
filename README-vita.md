# mpv-vita

The MPV media player port for PS Vita.


## Features

* **Hardware acceleration**: Be able to watch 1080P h264 videos.
* **Lightweight GUI**: Keep it simple, fast, and battery-efficient.
* **PC simulator**: Try and debug this project without a device.

## Screenshots

![files](https://raw.githubusercontent.com/fish47/mpv-vita/resources/files.png)
![player](https://raw.githubusercontent.com/fish47/mpv-vita/resources/player.png)


## Compilation

### Dependencies

#### PC simulator

* FFmpeg
* libass
* zlib
* OpenAL
* OpenGL
* FreeType
* GLFW
* Fontconfig (optional)

#### Vita homebrew

* vitasdk
* [FFmpeg-vita](https://github.com/fish47/FFmpeg-vita) (optional but recommended)

#### Options

| Name                 | Type | Default | Description |
| -------------------- | ---- | ------- | ----------- |
|`MPV_FFMPEG_LIBS_DIR` | Dir  | ""      | the location of external FFmpeg artifacts |
|`MPV_BUILD_SIMULATOR` | Bool | 0       | whether to build PC simulator |
