#include "dsp/SidPlayer.h"
#include "dsp/Node.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using NodeSynth::BlockSize;
using NodeSynth::EParamKind;
using NodeSynth::FProcessContext;
using NodeSynth::FSidPlayer;

namespace
{
	std::vector<uint8_t> BuildPsidV2Header(
		uint16_t LoadAddr, uint16_t InitAddr, uint16_t PlayAddr,
		uint16_t Songs = 1, uint16_t StartSong = 1, uint32_t Speed = 0,
		uint16_t Flags = 0)
	{
		std::vector<uint8_t> H(0x7C, 0);
		H[0] = 'P'; H[1] = 'S'; H[2] = 'I'; H[3] = 'D';
		H[4] = 0;    H[5] = 2;
		H[6] = 0;    H[7] = 0x7C;
		H[8] = static_cast<uint8_t>(LoadAddr >> 8); H[9] = static_cast<uint8_t>(LoadAddr & 0xFF);
		H[10] = static_cast<uint8_t>(InitAddr >> 8); H[11] = static_cast<uint8_t>(InitAddr & 0xFF);
		H[12] = static_cast<uint8_t>(PlayAddr >> 8); H[13] = static_cast<uint8_t>(PlayAddr & 0xFF);
		H[14] = static_cast<uint8_t>(Songs >> 8);    H[15] = static_cast<uint8_t>(Songs & 0xFF);
		H[16] = static_cast<uint8_t>(StartSong >> 8); H[17] = static_cast<uint8_t>(StartSong & 0xFF);
		H[18] = static_cast<uint8_t>((Speed >> 24) & 0xFF);
		H[19] = static_cast<uint8_t>((Speed >> 16) & 0xFF);
		H[20] = static_cast<uint8_t>((Speed >> 8) & 0xFF);
		H[21] = static_cast<uint8_t>(Speed & 0xFF);
		H[0x76] = static_cast<uint8_t>(Flags >> 8);
		H[0x77] = static_cast<uint8_t>(Flags & 0xFF);
		return H;
	}

	std::filesystem::path WriteTempSidFile(const std::vector<uint8_t>& Bytes)
	{
		std::random_device Rd;
		std::mt19937_64 Rng(Rd());
		auto Dir = std::filesystem::temp_directory_path() / "nodesynth_sid_tests";
		std::filesystem::create_directories(Dir);
		auto P = Dir / ("synth_" + std::to_string(Rng()) + ".sid");
		std::ofstream Out(P, std::ios::binary);
		Out.write(reinterpret_cast<const char*>(Bytes.data()),
			static_cast<std::streamsize>(Bytes.size()));
		return P;
	}
}

TEST_CASE("FSidPlayer: declares 1 audio + 28 control outputs", "[sid][player]")
{
	FSidPlayer P;
	const auto Outputs = P.GetOutputPorts();
	REQUIRE(Outputs.size() == 29);
	REQUIRE(Outputs[0].Name == "Out");
	REQUIRE(Outputs[0].Type == NodeSynth::EPortType::Audio);
	for (size_t I = 1; I < Outputs.size(); ++I)
	{
		REQUIRE(Outputs[I].Type == NodeSynth::EPortType::Control);
	}
}

TEST_CASE("FSidPlayer: silent + zero outputs when no file loaded", "[sid][player]")
{
	FSidPlayer P;
	P.Prepare(48000.0);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	P.Process(Ctx);

	const float* Audio = P.GetOutputBuffer(FSidPlayer::Output_Audio, 0);
	REQUIRE(Audio != nullptr);
	for (uint32_t I = 0; I < BlockSize; ++I)
	{
		REQUIRE(Audio[I] == 0.0f);
	}
}

TEST_CASE("FSidPlayer: load + run captures play-routine writes into Control outputs", "[sid][player]")
{
	// Synthetic tune: init writes nothing; play writes V1 freq lo/hi every
	// tick. Same shape as the PsidLoader integration test but exercised
	// through the FSidPlayer node.
	const uint8_t Code[] = {
		// Init at $1000:
		0x58, 0x60,                        // CLI; RTS  (PC = $1000)
		// Play at $1002:
		0xA9, 0x34, 0x8D, 0x00, 0xD4,      // LDA #$34; STA $D400
		0xA9, 0x12, 0x8D, 0x01, 0xD4,      // LDA #$12; STA $D401
		0x60,                              // RTS
	};
	std::vector<uint8_t> File = BuildPsidV2Header(
		/*LoadAddr*/ 0x1000,
		/*InitAddr*/ 0x1000,
		/*PlayAddr*/ 0x1002,
		/*Songs*/    1,
		/*StartSong*/ 1,
		/*Speed*/    0,    // VBI
		/*Flags*/    0x14); // PAL/6581
	File.insert(File.end(), Code, Code + sizeof(Code));
	const auto Path = WriteTempSidFile(File);

	FSidPlayer P;
	P.SetParamString(FSidPlayer::Param_File, Path.string());
	P.Prepare(48000.0);

	const auto Status = P.GetStatus();
	REQUIRE(Status.bLoaded);
	REQUIRE(Status.ErrorMessage.empty());

	// Run ~50 ms — well past one VBI tick at 50 Hz.
	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	const uint32_t Blocks = static_cast<uint32_t>(48000.0 * 0.05) / BlockSize;
	for (uint32_t B = 0; B < Blocks; ++B) { P.Process(Ctx); }

	// V1_Freq should have a non-zero value derived from $1234. The PAL
	// formula: Hz = (0x1234 * 985248) / 16777216 ≈ 273.7 Hz.
	const float* V1Freq = P.GetOutputBuffer(FSidPlayer::Output_V1_Freq, 0);
	REQUIRE(V1Freq != nullptr);
	REQUIRE_THAT(V1Freq[BlockSize - 1], Catch::Matchers::WithinAbs(273.7f, 5.0f));
}

TEST_CASE("FSidPlayer: bypass param zeros all outputs", "[sid][player]")
{
	FSidPlayer P;
	P.Prepare(48000.0);
	// Without loading anything, set bypass and check audio + a control output.
	P.SetParamValue(FSidPlayer::Param_Bypass, 1.0f);

	FProcessContext Ctx;
	Ctx.BlockSize = BlockSize;
	Ctx.SampleRate = 48000.0;
	P.Process(Ctx);

	for (uint32_t Out = 0; Out < FSidPlayer::Output_COUNT; ++Out)
	{
		const float* B = P.GetOutputBuffer(Out, 0);
		REQUIRE(B != nullptr);
		for (uint32_t I = 0; I < BlockSize; ++I)
		{
			REQUIRE(B[I] == 0.0f);
		}
	}
}

TEST_CASE("FSidPlayer: GetParamInfos exposes the file param as a String kind", "[sid][player]")
{
	FSidPlayer P;
	const auto Infos = P.GetParamInfos();
	REQUIRE(Infos.size() == FSidPlayer::Param_COUNT);
	REQUIRE(Infos[FSidPlayer::Param_File].Kind == EParamKind::String);
	REQUIRE(Infos[FSidPlayer::Param_Subtune].Kind == EParamKind::Float);
}
