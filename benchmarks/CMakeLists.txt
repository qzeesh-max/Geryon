add_executable(benchmark_page_transfer benchmark_page_transfer.cpp)

target_link_libraries(benchmark_page_transfer
    PRIVATE geryon
    PRIVATE benchmark::benchmark
    PRIVATE benchmark::benchmark_main
)
