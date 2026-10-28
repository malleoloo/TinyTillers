cmake_minimum_required(VERSION 3.10)
project(TinyTillers)

# Cerca automaticamente tutti i file .cpp dentro la cartella src
file(GLOB SOURCES "src/*.cpp")

add_executable(TinyTillers ${SOURCES})

# Percorsi di inclusione e librerie di MSYS2 UCRT64
target_include_directories(TinyTillers PRIVATE "C:/msys/ucrt64/include")
target_link_directories(TinyTillers PRIVATE "C:/msys/ucrt64/lib")

# Collegamento di Raylib, ENet e delle dipendenze di Windows
target_link_libraries(TinyTillers 
    raylib 
    enet 
    ws2_32 
    winmm 
    opengl32 
    gdi32
)  # <-- QUI MANCAVA LA PARENTESI TONDA DI CHIUSURA!

# Copia automatica della cartella assets accanto all'eseguibile
add_custom_command(TARGET TinyTillers POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:TinyTillers>/assets
)