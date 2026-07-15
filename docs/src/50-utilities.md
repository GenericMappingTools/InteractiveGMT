```@meta
CurrentModule = InteractiveGMT
```

# Utilities

## Checking Window Status

```julia
if isalive(fig)
    println("Window is open")
end
```

## Saving Screenshots

```julia
save_png(fig, "output.png")
```

## Waiting for Windows

```julia
fig = iview(G)
wait_windows()  # Blocks until all windows close
```

## Stereo 3D

```julia
stereo!(fig, "red-cyan")  # Enable anaglyph stereo
```

## Front-Door Dispatcher

Auto-detect file type:

```julia
fig = iview("data.nc")  # Auto-detects grid/image/dataset
```

## Demo

```julia
view_demo()  # Opens synthetic peaks demo
```
