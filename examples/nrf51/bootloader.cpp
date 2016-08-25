#include <bluetoe/services/bootloader.hpp>
#include <bluetoe/server.hpp>
#include <bluetoe/bindings/nrf51.hpp>
#include <bluetoe/link_layer/connection_event_callback.hpp>

#include <nrf.h>

static constexpr std::size_t flash_page_size    = 1024;
static constexpr unsigned    erase_page_time_ms = 23;
static constexpr unsigned    number_of_concurrent_flashs = 2;

namespace bb = bluetoe::bootloader;

class flash_handler {
public:
    bb::error_codes start_flash( std::uintptr_t address, const std::uint8_t* values, std::size_t size );
    bb::error_codes run( std::uintptr_t start_addr );
    bb::error_codes reset();
    std::pair< const std::uint8_t*, std::size_t > get_version();
    void read_mem( std::uintptr_t address, std::size_t size, std::uint8_t* destination );
    std::uint32_t checksum32( std::uintptr_t start_addr, std::size_t size );
    std::uint32_t checksum32( const std::uint8_t* start_addr, std::size_t size, std::uint32_t old_crc );
    std::uint32_t checksum32( std::uintptr_t start_addr );

    flash_handler();

    void ll_connection_event_happend();
private:
    std::uint32_t crc_table_[ 256 ];

    struct flash_queue_entry {
        std::uintptr_t      address;
        const std::uint8_t* values;
    };

    std::array< flash_queue_entry, number_of_concurrent_flashs > flash_entries_;
    unsigned next_flash_entry_;
    unsigned num_flash_entries_;
};

using gatt_definition = bluetoe::server<
    bluetoe::bootloader_service<
        bb::handler< flash_handler >,
        bb::page_size< flash_page_size >,
        bb::white_list<
            bb::memory_region< 0x6000, 0x40000 >
        >
    >
>;

/*
 * Change the default device address slightly, so that clients do not get confused by devices that
 * have the very same address, but totally different GATT structures
 */
struct address_generator {
    static constexpr bool is_random()
    {
        return true;
    }

    template < class Radio >
    static bluetoe::link_layer::random_device_address address( const Radio& r )
    {
        return bluetoe::link_layer::address::generate_static_random_address( ~r.static_random_address_seed() );
    }

    typedef bluetoe::link_layer::details::device_address_meta_type meta_type;
};

gatt_definition gatt_server;

bluetoe::nrf51<
    gatt_definition,
    bluetoe::link_layer::buffer_sizes< flash_page_size * 3, flash_page_size * 3 >,
    // TODO: advertise a larger maximum connection interval
    bluetoe::link_layer::connection_event_callback< gatt_definition, gatt_server /* erase_page_time_ms */ >,
    address_generator
> link_layer;

int main()
{
    for ( ;; )
    {
        link_layer.run( gatt_server );
    }
}

bb::error_codes flash_handler::start_flash( std::uintptr_t address, const std::uint8_t* values, std::size_t size )
{
    assert( num_flash_entries_ < number_of_concurrent_flashs );
    assert( size == flash_page_size );

    const auto next_entry = ( num_flash_entries_ + next_flash_entry_ ) % number_of_concurrent_flashs;
    flash_entries_[ next_entry ] = flash_queue_entry{ address, values };

    ++num_flash_entries_;

    return bb::error_codes::success;
}

bb::error_codes flash_handler::run( std::uintptr_t start_addr )
{
    return bb::error_codes::success;
}

bb::error_codes flash_handler::reset()
{
    return bb::error_codes::success;
}

std::pair< const std::uint8_t*, std::size_t > flash_handler::get_version()
{
    static const std::uint8_t version[] = "ble_flash 1.0";

    return std::pair< const std::uint8_t*, std::size_t >( &version[ 0 ], sizeof( version ) -1 );
}

void flash_handler::read_mem( std::uintptr_t address, std::size_t size, std::uint8_t* destination )
{
}

std::uint32_t flash_handler::checksum32( std::uintptr_t start_addr, std::size_t size )
{
    return checksum32( reinterpret_cast< const std::uint8_t* >( start_addr ), size, 0 );
}

std::uint32_t flash_handler::checksum32( const std::uint8_t* data, std::size_t size, std::uint32_t crc )
{
    crc = ~crc;

    for ( ; size; --size, ++data )
        crc = (crc >> 8) ^ crc_table_[ ( crc ^ *data ) & 0xFF ];

    return ~crc;
}

std::uint32_t flash_handler::checksum32( std::uintptr_t start_addr )
{
    std::uint8_t addr[ sizeof( start_addr ) ];

    for ( auto p = std::begin( addr ); p != std::end( addr ); ++p, start_addr = start_addr >> 8 )
        *p = start_addr & 0xff;

    return checksum32( std::begin( addr ), sizeof( addr ), 0 );
}

flash_handler::flash_handler()
    : next_flash_entry_( 0 )
    , num_flash_entries_( 0 )
{
    for ( std::uint32_t n = 0; n != sizeof( crc_table_ ) / sizeof( crc_table_[ 0 ] ); ++n )
    {
        std::uint32_t c = n;

        for ( std::uint32_t k = 0; k != 8; ++k )
            c = ( c & 1 )
                ? (0xEDB88320 ^ (c >> 1))
                : (c >> 1);

        crc_table_[ n ] = c;
    }
}

void wait_flash()
{
    while ( ( NRF_NVMC->READY & NVMC_READY_READY_Ready ) == 0 )
        ;
}

static void erase_flash( std::uintptr_t addr )
{
    NRF_NVMC->CONFIG = ( NRF_NVMC->CONFIG & ~NVMC_CONFIG_WEN_Msk ) | NVMC_CONFIG_WEN_Een;

    NRF_NVMC->ERASEPAGE = static_cast< std::uint32_t >( addr );

    wait_flash();

    NRF_NVMC->CONFIG = ( NRF_NVMC->CONFIG & ~NVMC_CONFIG_WEN_Msk );
}

static void flash_page( std::uintptr_t addr, const std::uint8_t* page )
{
    wait_flash();

    NRF_NVMC->CONFIG = ( NRF_NVMC->CONFIG & ~NVMC_CONFIG_WEN_Msk ) | NVMC_CONFIG_WEN_Wen;
    wait_flash();

    const std::uint32_t* source = reinterpret_cast< const std::uint32_t* >( page );
          std::uint32_t* target = reinterpret_cast< std::uint32_t* >( addr );

    for ( std::size_t count = flash_page_size / sizeof( std::uint32_t* ); count; --count )
    {
        *target = *source;

        ++source;
        ++target;
        wait_flash();
    }

    NRF_NVMC->CONFIG = ( NRF_NVMC->CONFIG & ~NVMC_CONFIG_WEN_Msk );
}

void flash_handler::ll_connection_event_happend()
{
    if ( num_flash_entries_ && ( NRF_NVMC->READY & NVMC_READY_READY_Ready ) != 0 )
    {
        const auto& entry = flash_entries_[ next_flash_entry_ ];
        erase_flash( entry.address );
        flash_page( entry.address, entry.values );

        --num_flash_entries_;
        next_flash_entry_ = ( next_flash_entry_ + 1 ) % number_of_concurrent_flashs;

        bb::end_flash( gatt_server );
    }
}
