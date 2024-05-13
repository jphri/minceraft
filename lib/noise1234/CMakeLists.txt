cmake_minimum_required(VERSION 3.1)
project(noise1234)

add_library(noise1234 STATIC noise1234.c)
target_include_directories(noise1234 PUBLIC include)
