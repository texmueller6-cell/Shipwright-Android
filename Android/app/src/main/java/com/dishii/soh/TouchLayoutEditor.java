package com.dishii.soh;

import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.constraintlayout.widget.ConstraintLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * Editor de layout dos controles de toque.
 *
 * Como usar no jogo: segure o botao pequeno de "mostrar/esconder" (canto inferior direito)
 * para entrar no modo de edicao. Toque num controle e arraste para mover; use a barra
 * "Tamanho" para redimensionar o controle selecionado e "Opacidade" para todos.
 * "Restaurar" volta ao layout original e "Pronto" salva e sai.
 *
 * O layout e salvo nas SharedPreferences (posicao como fracao da tela, entao funciona em
 * qualquer resolucao). Nada no XML original precisa mudar: tudo e feito com
 * translation/scale/alpha das views.
 */
public class TouchLayoutEditor {

    private static final String PREF = "tl_";
    private static final String PREF_ALPHA = "tl_alpha";
    private static final float MIN_SCALE = 0.5f;
    private static final float MIN_ALPHA = 0.2f;

    private static class Group {
        final String key;
        final String label;
        final View hit;                       // view usada para detectar o toque e o contorno
        final List<View> views = new ArrayList<>();
        float fx = 0f, fy = 0f;               // deslocamento como fracao da largura/altura
        float scale = 1f;

        Group(String key, String label, View hit, View... all) {
            this.key = key;
            this.label = label;
            this.hit = hit;
            for (View v : all) views.add(v);
        }
    }

    private final ConstraintLayout overlayRoot;
    private final ViewGroup buttonGroup;
    private final SharedPreferences prefs;
    private final List<Group> groups = new ArrayList<>();

    private boolean ready = false;
    private boolean editing = false;
    private boolean wasHidden = false;
    private float opacity = 1f;

    // UI de edicao
    private FrameLayout editLayer;
    private View highlight;
    private TextView selectedLabel;
    private SeekBar sizeBar;
    private SeekBar alphaBar;
    private Group selected;
    private Group dragging;
    private float lastX, lastY;

    public TouchLayoutEditor(ConstraintLayout overlayRoot, ViewGroup buttonGroup, SharedPreferences prefs) {
        this.overlayRoot = overlayRoot;
        this.buttonGroup = buttonGroup;
        this.prefs = prefs;

        View stick = overlayRoot.findViewById(R.id.left_joystick);
        View cross = overlayRoot.findViewById(R.id.imageView);

        groups.add(new Group("stick", "Analogico", stick, stick));
        groups.add(new Group("cbtn", "Botoes C", cross,
                cross,
                overlayRoot.findViewById(R.id.buttonDpadUp),
                overlayRoot.findViewById(R.id.buttonDpadDown),
                overlayRoot.findViewById(R.id.buttonDpadLeft),
                overlayRoot.findViewById(R.id.buttonDpadRight)));
        addSingle("a", "Botao A", R.id.buttonA);
        addSingle("b", "Botao B", R.id.buttonB);
        addSingle("x", "Botao X", R.id.buttonX);
        addSingle("y", "Botao Y", R.id.buttonY);
        addSingle("l", "Botao L", R.id.buttonLB);
        addSingle("r", "Botao R", R.id.buttonRB);
        addSingle("z", "Botao Z", R.id.buttonZ);
        addSingle("start", "Start", R.id.buttonStart);
        addSingle("back", "Back", R.id.buttonBack);

        // Espera o layout terminar para calcular os pontos de pivo e aplicar o layout salvo.
        buttonGroup.getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        if (buttonGroup.getWidth() == 0 || buttonGroup.getHeight() == 0) return;
                        buttonGroup.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                        computePivots();
                        load();
                        applyAll();
                        // O analogico precisa ficar acima da area de "olhar em volta",
                        // senao ela rouba o toque se voce mover o analogico para a direita.
                        groups.get(0).hit.bringToFront();
                        ready = true;
                    }
                });
    }

    private void addSingle(String key, String label, int id) {
        View v = overlayRoot.findViewById(id);
        groups.add(new Group(key, label, v, v));
    }

    // ---------------------------------------------------------------- geometria

    private int dp(int v) {
        return Math.round(v * overlayRoot.getResources().getDisplayMetrics().density);
    }

    private Rect screenRect(View v) {
        int[] l = new int[2];
        v.getLocationOnScreen(l);
        float w = v.getWidth() * v.getScaleX();
        float h = v.getHeight() * v.getScaleY();
        return new Rect(l[0], l[1], Math.round(l[0] + w), Math.round(l[1] + h));
    }

    /** Cada view escala em torno do centro do GRUPO, assim os botoes C continuam alinhados com a cruz. */
    private void computePivots() {
        int[] loc = new int[2];
        for (Group g : groups) {
            for (View v : g.views) {
                v.setScaleX(1f);
                v.setScaleY(1f);
                v.setTranslationX(0f);
                v.setTranslationY(0f);
            }
            Rect r = screenRect(g.hit);
            float cx = r.centerX();
            float cy = r.centerY();
            for (View v : g.views) {
                v.getLocationOnScreen(loc);
                v.setPivotX(cx - loc[0]);
                v.setPivotY(cy - loc[1]);
            }
        }
    }

    private void applyGroup(Group g) {
        float tx = g.fx * buttonGroup.getWidth();
        float ty = g.fy * buttonGroup.getHeight();
        for (View v : g.views) {
            v.setTranslationX(tx);
            v.setTranslationY(ty);
            v.setScaleX(g.scale);
            v.setScaleY(g.scale);
            v.setAlpha(opacity);
        }
    }

    private void applyAll() {
        for (Group g : groups) applyGroup(g);
    }

    // ---------------------------------------------------------------- persistencia

    private void load() {
        opacity = prefs.getFloat(PREF_ALPHA, 1f);
        for (Group g : groups) {
            g.fx = prefs.getFloat(PREF + g.key + "_x", 0f);
            g.fy = prefs.getFloat(PREF + g.key + "_y", 0f);
            g.scale = prefs.getFloat(PREF + g.key + "_s", 1f);
        }
    }

    private void save() {
        SharedPreferences.Editor e = prefs.edit();
        e.putFloat(PREF_ALPHA, opacity);
        for (Group g : groups) {
            e.putFloat(PREF + g.key + "_x", g.fx);
            e.putFloat(PREF + g.key + "_y", g.fy);
            e.putFloat(PREF + g.key + "_s", g.scale);
        }
        e.apply();
    }

    private void resetAll() {
        opacity = 1f;
        for (Group g : groups) {
            g.fx = 0f;
            g.fy = 0f;
            g.scale = 1f;
        }
        applyAll();
        syncSliders();
        updateHighlight();
    }

    // ---------------------------------------------------------------- modo de edicao

    public boolean isEditing() {
        return editing;
    }

    public void enterEditMode() {
        if (editing || !ready) return;
        editing = true;
        wasHidden = buttonGroup.getVisibility() != View.VISIBLE;
        buttonGroup.setVisibility(View.VISIBLE);
        buildEditLayer();
        editLayer.setVisibility(View.VISIBLE);
        editLayer.bringToFront();
        selected = null;
        syncSliders();
        updateHighlight();
        Toast.makeText(overlayRoot.getContext(),
                "Toque num controle e arraste para mover", Toast.LENGTH_SHORT).show();
    }

    private void exitEditMode() {
        save();
        editLayer.setVisibility(View.GONE);
        buttonGroup.setVisibility(wasHidden ? View.INVISIBLE : View.VISIBLE);
        editing = false;
        selected = null;
        dragging = null;
    }

    private Group hitTest(float x, float y) {
        Group best = null;
        long bestArea = Long.MAX_VALUE;
        int pad = dp(8);
        for (Group g : groups) {
            Rect r = screenRect(g.hit);
            r.inset(-pad, -pad);
            if (r.contains((int) x, (int) y)) {
                long area = (long) r.width() * r.height();
                if (area < bestArea) { // prefere o menor (botao em cima do analogico, etc.)
                    bestArea = area;
                    best = g;
                }
            }
        }
        return best;
    }

    private void moveGroup(Group g, float dxPx, float dyPx) {
        float w = buttonGroup.getWidth();
        float h = buttonGroup.getHeight();
        g.fx += dxPx / w;
        g.fy += dyPx / h;
        applyGroup(g);

        // Mantem o centro do controle dentro da tela.
        int[] bl = new int[2];
        buttonGroup.getLocationOnScreen(bl);
        Rect r = screenRect(g.hit);
        float cx = r.centerX();
        float cy = r.centerY();
        if (cx < bl[0]) g.fx += (bl[0] - cx) / w;
        if (cx > bl[0] + w) g.fx -= (cx - (bl[0] + w)) / w;
        if (cy < bl[1]) g.fy += (bl[1] - cy) / h;
        if (cy > bl[1] + h) g.fy -= (cy - (bl[1] + h)) / h;
        applyGroup(g);
    }

    private void syncSliders() {
        if (sizeBar == null) return;
        if (selected != null) {
            sizeBar.setEnabled(true);
            sizeBar.setProgress(Math.round((selected.scale - MIN_SCALE) * 100f));
            selectedLabel.setText(selected.label);
        } else {
            sizeBar.setEnabled(false);
            selectedLabel.setText("Nenhum controle selecionado");
        }
        alphaBar.setProgress(Math.round((opacity - MIN_ALPHA) * 100f));
    }

    private void updateHighlight() {
        if (highlight == null) return;
        if (selected == null) {
            highlight.setVisibility(View.GONE);
            return;
        }
        Rect r = screenRect(selected.hit);
        int[] l = new int[2];
        editLayer.getLocationOnScreen(l);
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) highlight.getLayoutParams();
        lp.width = Math.max(r.width(), 1);
        lp.height = Math.max(r.height(), 1);
        highlight.setLayoutParams(lp);
        highlight.setX(r.left - l[0]);
        highlight.setY(r.top - l[1]);
        highlight.setVisibility(View.VISIBLE);
    }

    // ---------------------------------------------------------------- UI da edicao (criada por codigo)

    private void buildEditLayer() {
        if (editLayer != null) return;
        android.content.Context ctx = overlayRoot.getContext();

        editLayer = new FrameLayout(ctx);
        editLayer.setBackgroundColor(Color.argb(90, 0, 0, 0));
        ConstraintLayout.LayoutParams full = new ConstraintLayout.LayoutParams(0, 0);
        full.leftToLeft = ConstraintLayout.LayoutParams.PARENT_ID;
        full.rightToRight = ConstraintLayout.LayoutParams.PARENT_ID;
        full.topToTop = ConstraintLayout.LayoutParams.PARENT_ID;
        full.bottomToBottom = ConstraintLayout.LayoutParams.PARENT_ID;
        overlayRoot.addView(editLayer, full);

        // contorno do controle selecionado
        highlight = new View(ctx);
        GradientDrawable border = new GradientDrawable();
        border.setColor(Color.TRANSPARENT);
        border.setStroke(dp(2), Color.YELLOW);
        border.setCornerRadius(dp(6));
        highlight.setBackground(border);
        highlight.setVisibility(View.GONE);
        editLayer.addView(highlight, new FrameLayout.LayoutParams(1, 1));

        // painel
        LinearLayout panel = new LinearLayout(ctx);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(12), dp(8), dp(12), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(220, 20, 20, 20));
        bg.setCornerRadius(dp(10));
        panel.setBackground(bg);
        panel.setClickable(true); // nao deixa o toque no painel virar arrastar

        selectedLabel = new TextView(ctx);
        selectedLabel.setTextColor(Color.WHITE);
        selectedLabel.setTextSize(14);
        panel.addView(selectedLabel);

        sizeBar = new SeekBar(ctx);
        sizeBar.setMax(Math.round((2.0f - MIN_SCALE) * 100f));
        sizeBar.setOnSeekBarChangeListener(new SimpleSeek() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                if (!fromUser || selected == null) return;
                selected.scale = MIN_SCALE + p / 100f;
                applyGroup(selected);
                updateHighlight();
            }
        });
        panel.addView(labeled(ctx, "Tamanho", sizeBar));

        alphaBar = new SeekBar(ctx);
        alphaBar.setMax(Math.round((1.0f - MIN_ALPHA) * 100f));
        alphaBar.setOnSeekBarChangeListener(new SimpleSeek() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                if (!fromUser) return;
                opacity = MIN_ALPHA + p / 100f;
                applyAll();
            }
        });
        panel.addView(labeled(ctx, "Opacidade", alphaBar));

        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.END);

        Button reset = new Button(ctx);
        reset.setText("Restaurar");
        reset.setOnClickListener(v -> resetAll());
        row.addView(reset);

        Button done = new Button(ctx);
        done.setText("Pronto");
        done.setOnClickListener(v -> exitEditMode());
        row.addView(done);
        panel.addView(row);

        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(
                dp(320), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        plp.topMargin = dp(8);
        editLayer.addView(panel, plp);

        // arrastar controles
        editLayer.setOnTouchListener((v, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    Group g = hitTest(e.getRawX(), e.getRawY());
                    dragging = g;
                    if (g != null) {
                        selected = g;
                        syncSliders();
                        updateHighlight();
                    }
                    lastX = e.getRawX();
                    lastY = e.getRawY();
                    return true;
                }
                case MotionEvent.ACTION_MOVE:
                    if (dragging != null) {
                        moveGroup(dragging, e.getRawX() - lastX, e.getRawY() - lastY);
                        lastX = e.getRawX();
                        lastY = e.getRawY();
                        updateHighlight();
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    dragging = null;
                    return true;
            }
            return true;
        });
    }

    private LinearLayout labeled(android.content.Context ctx, String text, SeekBar bar) {
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(ctx);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(12);
        t.setWidth(dp(80));
        row.addView(t);
        row.addView(bar, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private abstract static class SimpleSeek implements SeekBar.OnSeekBarChangeListener {
        @Override public void onStartTrackingTouch(SeekBar s) { }
        @Override public void onStopTrackingTouch(SeekBar s) { }
    }
}
