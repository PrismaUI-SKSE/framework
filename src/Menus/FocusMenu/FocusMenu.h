class FocusMenu : public RE::IMenu
{
public:
	static constexpr std::string_view MENU_NAME = "PrismaUI_FocusMenu";

	void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;
	RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;

	static RE::stl::owner<RE::IMenu*> Creator();

	static void Open();
	static void Close();
	static bool IsOpen();

private:
	FocusMenu();

	RE::GPtr<RE::GFxMovieView> _view;
};