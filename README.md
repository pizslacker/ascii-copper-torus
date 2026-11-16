# ascii-copper-torus

**ascii-copper-torus.c** renders a classic demoscene combination in the terminal: a 3D surface-lit ASCII torus (donut) with a real-time z-buffer, layered over scrolling 24-bit raster copper bars, a 3D starfield, and a sine-wave text scroller along the bottom.

### What It Renders
- **Z-Buffered 3D ASCII Torus**: Computes 3D torus surface coordinates and surface normals across rotation angles $A$ and $B$, projecting them into 2D terminal cells with depth testing (zbuf) and 12-level ASCII character shading (.,-~:;=!*#$@).
- **Amiga-Style Raster Copper Bars**: Three sine-wave horizontal light bars sweep behind the torus in the background, shading empty cells with subtle scanline characters (·, :, =).
- **3D Parallax Starfield**: Depth-layered ASCII stars (., +, *) stream outward from the center behind the 3D geometry.
- **Demoscene Sine-Wave Scroller**: A classic bottom marquee where each character oscillates vertically along a sine wave with a 24-bit rainbow gradient.

### License

This project is open-source software licensed under the GNU General Public License v3.0 (GPLv3). See the [LICENSE](LICENSE) file for terms and conditions.

Copyright (C) k!M/pizslacker 026
