/**
 * Roboton Task Board Firmware
 */

#pragma once

#include <M5Unified.h>

#include <esp_log.h>

#include <cstdint>

struct PbHubController
{
    const char * TAG = "PbHubController";

    static constexpr uint8_t DEFAULT_I2C_ADDR = 0x61;
    static constexpr uint32_t I2C_FREQ = 400000;

    enum class Operation : uint8_t
    {
        WRITE_IO0 = 0x00,
        WRITE_IO1 = 0x01,
        PWM_IO0 = 0x02,
        PWM_IO1 = 0x03,
        READ_IO0 = 0x04,
        READ_IO1 = 0x05,
        ANALOG_READ_IO0 = 0x06,
    };

    enum class Channel : uint8_t
    {
        CHANNEL_0 = 0x40,
        CHANNEL_1 = 0x50,
        CHANNEL_2 = 0x60,
        CHANNEL_3 = 0x70,
        CHANNEL_4 = 0x80,
        CHANNEL_5 = 0xA0,
    };

    PbHubController() noexcept
    : i2c_addr_(DEFAULT_I2C_ADDR)
    {

        bool init = M5.Ex_I2C.begin();

        if(init)
        {
            ESP_LOGI(TAG, "PbHubController::PbHubController() on address %d", i2c_addr_);
        }
        else
        {
            ESP_LOGE(TAG, "PbHubController::PbHubController() failed");
        }
    }

    bool check_status()
    {
        // Sample read on a register to check that I2C device is properly answering
        uint8_t value = 0;
        return read_operation(Channel::CHANNEL_0, Operation::READ_IO0, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    }

    bool read_digital_IO0(const Channel channel)
    {
        uint8_t value = 0;
        read_operation(channel, Operation::READ_IO0, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return value != 0;
    }

    bool read_digital_IO1(const Channel channel)
    {
        uint8_t value = 0;
        read_operation(channel, Operation::READ_IO1, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return value != 0;
    }

    uint16_t read_analog_IO0(const Channel channel)
    {
        uint16_t value = 0;
        read_operation(channel, Operation::ANALOG_READ_IO0, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return value;
    }

    void write_digital_IO0(const Channel channel, const bool value)
    {
        uint8_t data = value;
        read_operation(channel, Operation::WRITE_IO0, reinterpret_cast<uint8_t*>(&data), sizeof(data));
    }

    void write_digital_IO1(const Channel channel, const bool value)
    {
        uint8_t data = value;
        read_operation(channel, Operation::WRITE_IO1, reinterpret_cast<uint8_t*>(&data), sizeof(data));
    }

private:

    bool read_operation(const Channel channel, const Operation operation, uint8_t * data, const size_t length)
    {
        bool status = true;
        status &= M5.Ex_I2C.start(i2c_addr_, false, I2C_FREQ);
        status &= M5.Ex_I2C.write(static_cast<uint8_t>(channel) + static_cast<uint8_t>(operation));
        status &= M5.Ex_I2C.stop();

        status &= M5.Ex_I2C.start(i2c_addr_, true, I2C_FREQ);
        for (size_t i = 0; i < length; i++)
        {
            status &= M5.Ex_I2C.read(&data[i], 1);
        }
        status &= M5.Ex_I2C.stop();

        if(!status)
        {
            ESP_LOGE(TAG, "PbHubController::read_digital() failed");
        }

        return status;
    }

    bool write_operation(const Channel channel, const Operation operation, const uint8_t * data, const size_t length)
    {
        bool status = true;
        status &= M5.Ex_I2C.start(i2c_addr_, false, I2C_FREQ);
        status &= M5.Ex_I2C.write(static_cast<uint8_t>(channel) + static_cast<uint8_t>(operation));
        status &= M5.Ex_I2C.write(data, length);
        status &= M5.Ex_I2C.stop();

        if(!status)
        {
            ESP_LOGE(TAG, "PbHubController::write_digital() failed");
        }

        return status;
    }

    const uint8_t i2c_addr_ = 0;
};