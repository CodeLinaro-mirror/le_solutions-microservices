# CreateEvalLabelModelGrader

A LabelModelGrader object which uses a model to assign labels to each item in the evaluation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;label_model&#x60;. | 
**name** | **str** | The name of the grader. | 
**model** | **str** | The model to use for the evaluation. Must support structured outputs. | 
**input** | [**List[CreateEvalItem]**](CreateEvalItem.md) | A list of chat messages forming the prompt or context. May include variable references to the \&quot;item\&quot; namespace, ie {{item.name}}. | 
**labels** | **List[str]** | The labels to classify to each item in the evaluation. | 
**passing_labels** | **List[str]** | The labels that indicate a passing result. Must be a subset of labels. | 

## Example

```python
from openapi_client.models.create_eval_label_model_grader import CreateEvalLabelModelGrader

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalLabelModelGrader from a JSON string
create_eval_label_model_grader_instance = CreateEvalLabelModelGrader.from_json(json)
# print the JSON string representation of the object
print(CreateEvalLabelModelGrader.to_json())

# convert the object into a dict
create_eval_label_model_grader_dict = create_eval_label_model_grader_instance.to_dict()
# create an instance of CreateEvalLabelModelGrader from a dict
create_eval_label_model_grader_from_dict = CreateEvalLabelModelGrader.from_dict(create_eval_label_model_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


