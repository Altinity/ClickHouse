const labelsFor = require('./labels.cjs');

module.exports = async function apply(github, context) {
  const pr = context.payload.pull_request;
  const required = labelsFor(pr.base.ref);
  if (!required.length) return;

  const issues = github.rest.issues;
  const target = {...context.repo, issue_number: pr.number};
  const {data: issue} = await issues.get(target);
  const current = new Set(issue.labels.map(label => label.name.toLowerCase()));
  const missing = required.filter(name => !current.has(name));

  for (const name of missing) {
    try {
      await issues.getLabel({...context.repo, name});
    } catch (error) {
      if (error.status === 404) throw new Error(`Missing repository label: ${name}`, {cause: error});
      throw error;
    }
  }
  if (missing.length) await issues.addLabels({...target, labels: missing});

  if (!issue.assignees.length) {
    const login = pr.user.login;
    const {data} = await issues.addAssignees({...target, assignees: [login]});
    if (!data.assignees.some(user => user.login.toLowerCase() === login.toLowerCase())) {
      throw new Error(`GitHub did not assign @${login} to PR #${pr.number}`);
    }
  }
};
