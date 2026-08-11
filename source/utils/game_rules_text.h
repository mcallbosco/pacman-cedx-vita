/* Existing localized five-minute text and its ten-minute equivalent.
 * Shorter PC events reuse the pack's five-minute entries directly. */
static const struct { int id; const char *android, *pc; } pc_timer_text[] = {
    {22, "Ataque de pontos (5 min.)", "Ataque de pontos (10 min.)"},
    {80, "Faça mais pontos em 5 minutos!", "Faça mais pontos em 10 minutos!"},
    {22, "Punkteangriff (5 min.)", "Punkteangriff (10 min.)"},
    {80, "Mach die meisten Punkte in 5 Minuten!", "Mach die meisten Punkte in 10 Minuten!"},
    {22, "Score Attack (5 min.)", "Score Attack (10 min.)"},
    {80, "Score the most points in 5 minutes!", "Score the most points in 10 minutes!"},
    {22, "Ataque de puntuación (5 min.)", "Ataque de puntuación (10 min.)"},
    {80, "¡Consigue más puntos en 5 minutos!", "¡Consigue más puntos en 10 minutos!"},
    {22, "Attaque de score (5 min)", "Attaque de score (10 min)"},
    {80, "Faites le meilleur score en 5 minutes !", "Faites le meilleur score en 10 minutes !"},
    {22, "Attacco di punti (5 min.)", "Attacco di punti (10 min.)"},
    {80, "Fai il punteggio più alto in 5 minuti!", "Fai il punteggio più alto in 10 minuti!"},
    {22, "スコアアタック  (5分)", "スコアアタック  (10分)"},
    {80, "５分間のスコアアタック！", "１０分間のスコアアタック！"},
    {22, "스코어 어택 (5분)", "스코어 어택 (10분)"},
    {80, "5분 안에 가장 많은 점수를 획득하세요!", "10분 안에 가장 많은 점수를 획득하세요!"},
    {22, "Охота за очками (5 мин)", "Охота за очками (10 мин)"},
    {80, "Наберите как можно больше очков за 5 минут!", "Наберите как можно больше очков за 10 минут!"},
    {22, "分数挑战(5 分钟)", "分数挑战(10 分钟)"},
    {80, "在 5 分钟内获得最高分！", "在 10 分钟内获得最高分！"},
};
