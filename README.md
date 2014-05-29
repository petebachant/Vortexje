Vortexje
========
Vortexje is an open-source implementation of the source-doublet panel method. Its features include:

  * Transparent C++ API, easily integrated with other simulation environments
    (Baayen & Heinz GmbH offers integration with Modelica as a service.)
  * Gmsh file I/O
  * Optional dynamic wake emission.

Dependencies
------------
 * Eigen3, a C++ template library for linear algebra:
   http://eigen.tuxfamily.org/
 * CMake, to generate the build files:
   http://cmake.org/
 * Optionally Doxygen, to generate the documentation:
   http://www.stack.nl/~dimitri/doxygen/
   
Installation
------------
 * Run `CMake .` to generate the build files.
 * Build Vortexje using the chosen build system, e.g., by executing `make`.
 
See the following website for details on the CMake build process:
http://www.cmake.org/cmake/help/runningcmake.html
