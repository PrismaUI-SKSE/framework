(function () {
    function updateOutput(input, output) {
        output.textContent = input.value;
        console.info('[PrismaUITest] echo text updated: ' + input.value);
    }

    window.addEventListener('DOMContentLoaded', function () {
        const input = document.getElementById('echo-input');
        const output = document.getElementById('echo-output');
        const returnFocus = document.getElementById('return-focus');
        if (!input || !output || !returnFocus) {
            console.error('[PrismaUITest] echo view missing input, output, or return focus element');
            return;
        }

        updateOutput(input, output);
        input.addEventListener('input', function () {
            updateOutput(input, output);
        });

        returnFocus.addEventListener('click', function () {
            console.info('[PrismaUITest] echo return focus requested');
            if (typeof window.prismaApiTestReturnFocus === 'function') {
                window.prismaApiTestReturnFocus('focus:main');
            } else {
                console.warn('[PrismaUITest] prismaApiTestReturnFocus listener is unavailable');
            }
        });
    });
})();
