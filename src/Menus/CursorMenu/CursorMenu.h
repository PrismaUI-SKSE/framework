class CursorMenuEx : public RE::CursorMenu
{
public:
	void AdvanceMovie_Hook(float a_interval, std::uint32_t a_currentTime);
	RE::UI_MESSAGE_RESULTS ProcessMessage_Hook(RE::UIMessage& a_message);

	static void InstallHook();

	using AdvanceMovie_t = decltype(&RE::CursorMenu::AdvanceMovie);
	static inline REL::Relocation<AdvanceMovie_t> _AdvanceMovie;

	using ProcessMessage_t = decltype(&RE::CursorMenu::ProcessMessage);
	static inline REL::Relocation<ProcessMessage_t> _ProcessMessage;
};