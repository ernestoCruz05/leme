#ifndef LEME_CORE_GATE_H
#define LEME_CORE_GATE_H

#include <stdbool.h>

#include "shell/sticky.h"
#include "workspace/tag.h"

struct leme_view;
struct leme_tags;

/*
 * Pontos onde um teste pode obrigar uma falha que a máquina não produz a
 * pedido: uma alocação sem memória, uma captura recusada, uma reconciliação
 * de saídas falhada. Nulos em produção, e é essa a leitura correcta — o
 * compositor nunca os preenche.
 *
 * Ficam aqui juntos em vez de espalhados pelos ficheiros que os consultam:
 * quem lê encontra o mecanismo inteiro num sítio só.
 */
extern bool (*leme_gate_ownership_prepare)(const struct leme_view *);
extern bool (*leme_gate_tags_prepare)(enum leme_tags_prepare_checkpoint,
    const struct leme_view *, const struct leme_tags *);
extern bool (*leme_gate_capture_accept)(void);
extern bool (*leme_gate_output_reconcile)(void);
extern bool (*leme_gate_sticky_output_prepare)(
    enum leme_sticky_output_prepare_checkpoint);
extern bool (*leme_gate_sticky_unmap_prepare)(
    enum leme_sticky_unmap_prepare_checkpoint);

#endif
