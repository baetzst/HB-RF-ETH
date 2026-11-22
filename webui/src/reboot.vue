<template>
  <b-card
    :header="$t('title')"
    header-tag="h6"
    header-bg-variant="secondary"
    header-text-variant="white"
    class="mb-3"
  >
    <b-form @submit.stop.prevent>
      <p>{{ $t("description") }}</p>
      <b-alert variant="info" :show="state === 'rebooting'">{{ $t("rebooting") }}</b-alert>
      <b-alert variant="success" :show="state === 'online'">{{ $t("online") }}</b-alert>
      <b-alert variant="danger" :show="state === 'error'">{{ $t("error") }}</b-alert>
      <b-form-group label-cols-sm="9">
        <b-button
          variant="warning"
          block
          :disabled="state === 'rebooting'"
          @click="rebootClick"
        >{{ $t("reboot") }}</b-button>
      </b-form-group>
    </b-form>
  </b-card>
</template>

<script>
import Vue from "vue";

import Axios from "axios";

import VueI18n from "vue-i18n";
Vue.use(VueI18n);

import { FormPlugin } from "bootstrap-vue/esm/components/form";
Vue.use(FormPlugin);

import { FormGroupPlugin } from "bootstrap-vue/esm/components/form-group";
Vue.use(FormGroupPlugin);

import { ButtonPlugin } from "bootstrap-vue/esm/components/button";
Vue.use(ButtonPlugin);

import { AlertPlugin } from "bootstrap-vue/esm/components/alert";
Vue.use(AlertPlugin);

import { CardPlugin } from "bootstrap-vue/esm/components/card";
Vue.use(CardPlugin);

export default {
  name: "Reboot",
  data() {
    return {
      // idle, rebooting, online, error
      state: "idle"
    };
  },
  methods: {
    rebootClick() {
      if (!window.confirm(this.$t("confirm"))) return;

      var self = this;
      this.state = "rebooting";

      this.$store.dispatch("device/reboot").then(
        () => {
          // the module needs some seconds to shut down, then poll until it is back
          self.pollTimer = setTimeout(self.poll, 5000);
        },
        () => {
          self.state = "error";
        }
      );
    },
    poll() {
      var self = this;
      Axios.get("/sysinfo.json", { timeout: 2000 }).then(
        () => {
          self.state = "online";
          self.$store.dispatch("sysInfo/update");
        },
        () => {
          self.pollTimer = setTimeout(self.poll, 2000);
        }
      );
    }
  },
  beforeDestroy() {
    clearTimeout(this.pollTimer);
  },
  i18n: {
    locale: navigator.language,
    fallbackLocale: "en",
    messages: {
      de: {
        title: "Neustart",
        description:
          "Startet das Modul neu, damit geänderte Einstellungen oder eine neue Firmware aktiv werden. Danach baut das Modul die Verbindung zur CCU nicht automatisch wieder auf, die CCU-Software muss ebenfalls neu gestartet werden.",
        reboot: "Modul neu starten",
        confirm: "Das Modul jetzt neu starten?",
        rebooting: "Das Modul startet neu. Bitte warten ...",
        online:
          "Das Modul ist wieder erreichbar. Bitte starten Sie jetzt die CCU-Software neu.",
        error: "Der Neustart konnte nicht ausgelöst werden."
      },
      en: {
        title: "Restart",
        description:
          "Restarts the module so that changed settings or a new firmware take effect. Afterwards the module does not reconnect to the CCU automatically, so the CCU software has to be restarted as well.",
        reboot: "Restart module",
        confirm: "Restart the module now?",
        rebooting: "The module is restarting. Please wait ...",
        online:
          "The module is reachable again. Please restart the CCU software now.",
        error: "The restart could not be triggered."
      }
    }
  }
};
</script>

<style lang="css">
</style>
